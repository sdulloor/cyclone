// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include "cyclone.hpp"
#include "libcyclone.hpp"
#include "dispatcher_layout.hpp"
#include "../core/clock.hpp"
#include "cyclone_comm.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <boost/thread.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include<libpmemobj.h>
#include "dispatcher_exec.hpp"
#include "timeouts.hpp"

static void *cyclone_handle;
static boost::property_tree::ptree pt;

struct client_ro_state_st {
  unsigned long committed_txid;
  char * last_return_value;
  int last_return_size;
  
} client_ro_state [MAX_CLIENTS];


static PMEMobjpool *state;
static rpc_callback_t execute_rpc;
static rpc_gc_callback_t gc_rpc;
static bool client_blocked[MAX_CLIENTS];
static int me;
static unsigned long last_global_txid;
static unsigned long last_global_ro_txid = 0UL;
// Linked list of RPCs yet to be completed
static rpc_info_t * volatile pending_rpc_head;
static rpc_info_t * volatile pending_rpc_tail;

static volatile unsigned long list_lock = 0;

static void lock_rpc_list()
{
  // TEST + TEST&SET
  do {
    while(list_lock != 0);
  } while(!__sync_bool_compare_and_swap(&list_lock, 0, 1));
  __sync_synchronize();
}

static void unlock_rpc_list()
{
  __sync_synchronize();
  list_lock = 0;
  __sync_synchronize();
}

static rpc_info_t * locate_rpc(unsigned long global_txid, 
			       bool keep_lock = false)
{
  rpc_info_t *rpc_info;
  rpc_info_t *hit = NULL;
  lock_rpc_list();
  rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(rpc_info->rpc->global_txid == global_txid) {
      hit = rpc_info;
    } // There can be failed versions earlier in the chain
    rpc_info = rpc_info->next;
  }
  if(!keep_lock) {
    unlock_rpc_list(); 
  }
  // Note: assume gc will not remove this entry
  return hit;
}

static void dump_active_list()
{
  rpc_info_t *rpc_info;
  rpc_info_t *hit = NULL;
  lock_rpc_list();
  rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    BOOST_LOG_TRIVIAL(info) << "ACTIVE  " << rpc_info->rpc->global_txid;
    rpc_info = rpc_info->next;
  }
  unlock_rpc_list(); 
}

static int get_max_client_txid(int client_id)
{
  int max_client_txid = 0;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  lock_rpc_list();
  rpc_info_t *rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(rpc_info->rpc->client_id == client_id) {
      if(rpc_info->rpc->client_txid > max_client_txid) {
	max_client_txid = rpc_info->rpc->client_txid;
      }
    }
    rpc_info = rpc_info->next;
  }
  unlock_rpc_list();
  int last_rw_txid = D_RO(root)->client_state[client_id].committed_txid;
  int last_ro_txid = client_ro_state[client_id].committed_txid;
  if(last_rw_txid > max_client_txid) {
    max_client_txid = last_rw_txid;
  }
  if(last_ro_txid > max_client_txid) {
    max_client_txid = last_ro_txid;
  }
  return max_client_txid;
}

// This function must be executed in the context of a tx
static void mark_done(const rpc_t *rpc,
		      const void* ret_value,
		      const int ret_size)
{
  int client_id = rpc->client_id;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  struct client_state_st *cstate = &D_RW(root)->client_state[client_id];
  pmemobj_tx_add_range_direct(cstate, sizeof(struct client_state_st));
  if(!TOID_IS_NULL(cstate->last_return_value)) {
    TX_FREE(cstate->last_return_value);
  }
  if(ret_size > 0) {
    cstate->last_return_value = TX_ALLOC(char, ret_size);
    TX_MEMCPY(D_RW(cstate->last_return_value), ret_value, ret_size);
  }
  else {
    TOID_ASSIGN(cstate->last_return_value, OID_NULL);
  }
  cstate->last_return_size = ret_size;
  unsigned long *global_txid_ptr = &D_RW(root)->committed_global_txid;
  pmemobj_tx_add_range_direct(global_txid_ptr, sizeof(unsigned long));
  *global_txid_ptr = rpc->global_txid;
  __sync_synchronize(); // Main thread can return this value now
  cstate->committed_txid = rpc->client_txid;
}

void exec_rpc_internal(rpc_info_t *rpc)
{
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  bool aborted = false;
  TX_BEGIN(state) {
    if(rpc->rpc->flags & RPC_FLAG_PREREPLICATE) {
      while(!rpc->rep_success && !rpc->rep_failed);
    }
    if(!rpc->rep_failed) {
      rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			    rpc->len - sizeof(rpc_t),
			    &rpc->ret_value);
    }
    else {
      rpc->sz = 0;
    }
    if(rpc->rpc->flags & RPC_FLAG_POSTREPLICATE) {
      rpc->need_replication = true;
    }
    while(!rpc->rep_success && !rpc->rep_failed);
    if(rpc->rep_success) {
      mark_done(rpc->rpc, rpc->ret_value, rpc->sz);
    }
    else {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    aborted= true;
  } TX_END
  if(aborted) { // cleanup
    rpc->sz = 0;
    // Wait for replication to finish
    while(!rpc->rep_success && !rpc->rep_failed);
    TX_BEGIN(state) {
      unsigned long *global_txid_ptr = &D_RW(root)->committed_global_txid;
      pmemobj_tx_add_range_direct(global_txid_ptr, sizeof(unsigned long));
      *global_txid_ptr = rpc->rpc->global_txid;
      if(rpc->rep_success) { // User tx aborted 
	struct client_state_st *cstate = 
	  &D_RW(root)->client_state[rpc->rpc->client_id];
	pmemobj_tx_add_range_direct(cstate, sizeof(struct client_state_st));
	if(!TOID_IS_NULL(cstate->last_return_value)) {
	  TX_FREE(cstate->last_return_value);
	  
	}
	TOID_ASSIGN(cstate->last_return_value, OID_NULL);
	cstate->last_return_size = 0;
	__sync_synchronize();
	cstate->committed_txid = rpc->rpc->client_txid;
      }
    } TX_ONABORT{
      BOOST_LOG_TRIVIAL(fatal) << "Dispatcher tx abort !\n";
      exit(-1);
    } TX_END
  }
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}


void exec_rpc_internal_ro(rpc_info_t *rpc)
{
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			rpc->len - sizeof(rpc_t),
			&rpc->ret_value);
  rpc->rep_success = true; // No replication needed
  struct client_ro_state_st *cstate = &client_ro_state[rpc->rpc->client_id];
  if(cstate->last_return_value != NULL) {
    free(cstate->last_return_value);
    cstate->last_return_size = 0;
  }
  if(rpc->sz > 0) {
    cstate->last_return_value = (char *)malloc(rpc->sz);
    memcpy(cstate->last_return_value,
	   rpc->ret_value,
	   rpc->sz);
    cstate->last_return_size = rpc->sz;
    __sync_synchronize();
    cstate->committed_txid = rpc->rpc->client_txid;
  }
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}

static unsigned char tx_buffer[DISP_MAX_MSGSIZE];
static unsigned char rx_buffer[DISP_MAX_MSGSIZE];
static cyclone_switch *router;

static void gc_pending_rpc_list(bool is_master)
{
  rpc_info_t **rpcp, *rpc, *deleted;
  void *cookie;
  deleted = NULL;
  lock_rpc_list();
  rpcp = &pending_rpc_head;
  while((*rpcp) != NULL) {
    rpc = *rpcp;
    if(rpc->need_replication && is_master) {
      cookie = cyclone_add_entry(cyclone_handle, rpc_req, sz);
      if(cookie != NULL) {
	free(cookie);
      }
      rpc->need_replication = false;
    }
    if(rpc->complete) {
      *rpcp = rpc->next;
      rpc->next = deleted;
      deleted = rpc;
    }
    else {
      rpcp = &(rpc->next);
    }
  }
  if(pending_rpc_head == NULL) {
    pending_rpc_tail = NULL;
  }
  unlock_rpc_list();
  rpc_t *rpc_rep = (rpc_t *)tx_buffer;
  unsigned long rep_sz;
  while(deleted) {
    tmp = deleted;
    deleted = deleted->next;    
    if(client_blocked[tmp->rpc->client_id]) {
      rpc_rep->client_id   = tmp->rpc->client_id;
      rpc_rep->client_txid = tmp->rpc->client_txid;
      rep_sz = sizeof(rpc_t);
      if(tmp->rep_failed) {
	rpc_rep->code = RPC_REP_UNKNOWN;
      }
      else {
	rpc_rep->code = RPC_REP_COMPLETE;
	if(tmp->sz > 0) {
	  memcpy(&rpc_rep->payload,
		 (void *)tmp->ret_value,
		 tmp->sz);
	  rep_sz += tmp->sz;
	}
      }
      client_blocked[tmp->rpc->client_id] = false;
      cyclone_tx(router->output_socket(tmp->rpc->client_id), 
		 tx_buffer, 
		 rep_sz, 
		 "Dispatch reply");
    }
    if(tmp->sz != 0) {
      gc_rpc(tmp->ret_value);
    }
    delete tmp->rpc;
    delete tmp;
  }
}

static void issue_rpc(const rpc_t *rpc, int len)
{
  rpc_info_t *rpc_info = new rpc_info_t;
  rpc_info->rep_success = false;
  rpc_info->rep_failed  = false;
  rpc_info->need_replication = false;
  rpc_info->complete    = false;
  rpc_info->len = len;
  rpc_info->rpc = (rpc_t *)(new char[len]);
  memcpy(rpc_info->rpc, rpc, len);
  rpc_info->next = NULL;
  lock_rpc_list();
  if(pending_rpc_head == NULL) {
    pending_rpc_head = pending_rpc_tail = rpc_info;
  }
  else {
    pending_rpc_tail->next = rpc_info;
    pending_rpc_tail = rpc_info;
  }
  unlock_rpc_list();
  exec_rpc(rpc_info);
  __sync_synchronize();
}

static void event_seen(const rpc_t *rpc)
{
  if(rpc->global_txid > last_global_txid) {
    last_global_txid = rpc->global_txid;
  }
}

void cyclone_commit_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *rpc_info;
  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc_info = locate_rpc(rpc->global_txid, true);
  if(rpc_info == NULL) {
    if(rpc->global_txid > D_RO(root)->committed_global_txid) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to locate replicated RPC id = "
	<< rpc->global_txid
	<< " last seen global txid = "
	<< last_global_txid
	<< " committed global txid "
	<< D_RO(root)->committed_global_txid;
      dump_active_list();
      exit(-1);
    }
  }
  else {
    rpc_info->rep_success = true;
    __sync_synchronize();
  }
  unlock_rpc_list();
}

// Note: node cannot become master while this function is in progress
void cyclone_rep_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  bool issue_it;
  rpc_info_t *match;
  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  event_seen(rpc);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  issue_it = (D_RO(root)->committed_global_txid < rpc->global_txid); // not committed
  // not already issued
  match = locate_rpc(rpc->global_txid, true);
  if(match != NULL) {
    issue_it =  issue_it && match->rep_failed;
  }
  unlock_rpc_list();
  if(issue_it) {
    issue_rpc(rpc, len);
  }
}

// Note: cyclone pop_cb cannot be called once the node becomes a master
void cyclone_pop_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *rpc_info;
  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  rpc_info = locate_rpc(rpc->global_txid, true);
  if(rpc_info == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to locate failed replication RPC !";
    exit(-1);
  }
  rpc_info->rep_failed = true;
  unlock_rpc_list();
  __sync_synchronize();
}


struct dispatcher_loop {
  void *zmq_context;
  int clients;

  void send_kicker()
  {
    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_req->code = RPC_REQ_MARKER;
    void *cookie = cyclone_add_entry(cyclone_handle, rpc_req, sizeof(rpc_t));
    if(cookie != NULL) {
      free(cookie);
    }
  }
  
  void handle_rpc(unsigned long sz)
  {
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    unsigned long last_committed;
    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_t *rpc_rep = (rpc_t *)tx_buffer;
    rpc_info_t *rpc_info;
    unsigned long rep_sz = 0;
    void *cookie;
    unsigned long last_tx_committed;
    switch(rpc_req->code) {
    case RPC_REQ_LAST_TXID:
      if(!cyclone_is_leader(cyclone_handle)) {
	rep_sz = sizeof(rpc_t);
	rpc_rep->code = RPC_REP_INVSRV;
	rpc_rep->master = cyclone_get_leader(cyclone_handle);
      }
      else {
	rep_sz = sizeof(rpc_t);
	rpc_rep->code = RPC_REP_COMPLETE;
	rpc_rep->client_txid = rpc_req->client_txid;
	rpc_rep->last_client_txid = get_max_client_txid(rpc_req->client_id);
      }
      break;
    case RPC_REQ_FN:
      rpc_rep->client_id   = rpc_req->client_id;
      if(get_max_client_txid(rpc_req->client_id) >= rpc_req->client_txid) {
	// Repeat request - ignore
      }
      else if((rpc_req->flags & RPC_FLAG_RO) == 0) {
	rpc_rep->client_txid = rpc_req->client_txid;
	// Initiate replication
	rpc_req->global_txid = (++last_global_txid);
	issue_rpc(rpc_req, sz);
	if((rpc_req->flags & RPC_FLAG_POSTREPLICATE) == 0) {
	  cookie = cyclone_add_entry(cyclone_handle, rpc_req, sz);
	  if(cookie != NULL) {
	    event_seen(rpc_req);
	    rep_sz = sizeof(rpc_t);
	    rpc_rep->code = RPC_REP_PENDING;
	    free(cookie);
	  }
	  else {
	    // Roll this back
	    cyclone_pop_cb(NULL, (const unsigned char *)rpc_req, sz);
	    rep_sz = sizeof(rpc_t);
	    rpc_rep->code = RPC_REP_INVSRV;
	    rpc_rep->master = cyclone_get_leader(cyclone_handle);
	  }
	}
      }
      else {
	rpc_rep->client_txid = rpc_req->client_txid;
	// Distinguish ro txids from rw txids
	rpc_req->global_txid = (++last_global_ro_txid) + (1UL << 63);
	issue_rpc(rpc_req, sz);
	rep_sz = sizeof(rpc_t);
	rpc_rep->code = RPC_REP_PENDING;
      }
      
      break;
    case RPC_REQ_STATUS:
    case RPC_REQ_STATUS_BLOCK:
      if(!cyclone_is_leader(cyclone_handle)) {
	rep_sz = sizeof(rpc_t);
	rpc_rep->code = RPC_REP_INVSRV;
	rpc_rep->master = cyclone_get_leader(cyclone_handle);
      }
      else {
	rpc_rep->client_id   = rpc_req->client_id;
	rpc_rep->client_txid = rpc_req->client_txid;
	rep_sz = sizeof(rpc_t);
	if(get_max_client_txid(rpc_req->client_id) < rpc_req->client_txid) {
	  rpc_rep->code = RPC_REP_UNKNOWN;
	}
	else {
	  int last_rw_txid = D_RO(root)->client_state[rpc_req->client_id].committed_txid;
	  int last_ro_txid = client_ro_state[rpc_req->client_id].committed_txid;
	  if(last_rw_txid < rpc_req->client_txid &&
	     last_ro_txid < rpc_req->client_txid ) {
	    rpc_rep->code = RPC_REP_PENDING;
	  }
	  else if(last_rw_txid == rpc_req->client_txid) {
	    const struct client_state_st * s =
	      &D_RO(root)->client_state[rpc_req->client_id];
	    if(s->last_return_size > 0) {
	      memcpy(&rpc_rep->payload,
		     (void *)D_RO(s->last_return_value),
		     s->last_return_size);
	      rep_sz += s->last_return_size;
	    }
	    rpc_rep->code = RPC_REP_COMPLETE;
	  }
	  else if(last_ro_txid == rpc_req->client_txid) {
	    if(client_ro_state[rpc_req->client_id].last_return_size > 0) {
	      memcpy(&rpc_rep->payload,
		     client_ro_state[rpc_req->client_id].last_return_value,
		     client_ro_state[rpc_req->client_id].last_return_size);
	      rep_sz += client_ro_state[rpc_req->client_id].last_return_size;
	    }
	    rpc_rep->code = RPC_REP_COMPLETE;
	  }
	  else { // Don't remember old results
	    rpc_rep->code = RPC_REP_COMPLETE;
	  }
	}
      }
      break;
    default:
      BOOST_LOG_TRIVIAL(fatal) << "DISPATCH: unknown code";
      exit(-1);
    }
    if(rep_sz > 0) {
      cyclone_tx(router->output_socket(rpc_req->client_id), 
		 tx_buffer, 
		 rep_sz, 
		 "Dispatch reply");
    }
  }

  void operator ()()
  {
    rtc_clock clock;
    bool is_master = false;
    clock.start();
    while(true) {
      for(int i=0;i<clients;i++) {
	unsigned long sz = cyclone_rx_noblock(router->input_socket(i),
					      rx_buffer,
					      DISP_MAX_MSGSIZE,
					      "DISP RCV");
	if(sz == -1) {
	  continue;
	}
	handle_rpc(sz);
      }
      clock.stop();
      if(clock.elapsed_time() >= PERIODICITY) {
	// Leadership change ?
	if(cyclone_is_leader(cyclone_handle)) {
	  gc_pending_rpc_list(true);
	  if(!is_master) {
	    is_master = true;
	    send_kicker();
	  }
	}
	else {
	  gc_pending_rpc_list(false);
	  is_master = false;
	}
	clock.reset();
      }
      clock.start();
    }
  }
};

static dispatcher_loop * dispatcher_loop_obj;

void dispatcher_start(const char* config_path,
		      rpc_callback_t rpc_callback,
		      rpc_gc_callback_t gc_callback,
		      rpc_nvheap_setup_callback_t nvheap_setup_callback,
		      int me,
		      int replicas,
		      int clients)
{
  boost::property_tree::read_ini(config_path, pt);
  // Load/Setup state
  std::string file_path = pt.get<std::string>("dispatch.filepath");
  char me_str[100];
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  dispatcher_exec_startup();
  if(access(file_path.c_str(), F_OK)) {
    state = pmemobj_create(file_path.c_str(),
			   POBJ_LAYOUT_NAME(disp_state),
			   sizeof(disp_state_t) + PMEMOBJ_MIN_POOL,
			   0666);
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool for dispatcher:"
	<< strerror(errno);
      exit(-1);
    }
  
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    TX_BEGIN(state) {
      TX_ADD(root); // Add everything
      for(int i = 0;i < MAX_CLIENTS;i++) {
	D_RW(root)->client_state[i].committed_txid    = 0UL;
	D_RW(root)->client_state[i].last_return_size  = 0;
	TOID_ASSIGN(D_RW(root)->client_state[i].last_return_value, OID_NULL);
      }
      D_RW(root)->committed_global_txid = 0;
      D_RW(root)->nvheap_root = nvheap_setup_callback(TOID_NULL(char), state);
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to setup dispatcher state:"
	<< strerror(errno);
      exit(-1);
    } TX_END
  }
  else {
    state = pmemobj_open(file_path.c_str(),
			 "disp_state");
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to open pmemobj pool for dispatcher state:"
	<< strerror(errno);
      exit(-1);
    }
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    TX_BEGIN(state) {
      D_RW(root)->nvheap_root = nvheap_setup_callback(D_RO(root)->nvheap_root, state);
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Application unable to recover state:"
	<< strerror(errno);
      exit(-1);
    } TX_END
    BOOST_LOG_TRIVIAL(info) << "DISPATCHER: Recovered state";
  }

  // Setup RO state
  for(int i = 0;i < MAX_CLIENTS;i++) {
    client_ro_state[i].committed_txid    = 0UL;
    client_ro_state[i].last_return_size  = 0;
    client_ro_state[i].last_return_value = NULL;
  }
  
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  BOOST_LOG_TRIVIAL(info) << "committed global txid = " 
			  << D_RO(root)->committed_global_txid;
  for(int i=0;i<MAX_CLIENTS;i++) {
    client_blocked[i]          = false;
  }
  execute_rpc = rpc_callback;
  gc_rpc      = gc_callback;
  last_global_txid = 0; // Count up from zero, always
  pending_rpc_head = pending_rpc_tail = NULL;
  // Boot cyclone -- this can lead to rep cbs on recovery
  cyclone_handle = cyclone_boot(config_path,
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_commit_cb,
				me,
				replicas,
				NULL);
  // Listen on port
  void *zmq_context = zmq_init(1);
  dispatcher_loop_obj    = new dispatcher_loop();
  dispatcher_loop_obj->zmq_context = zmq_context;
  dispatcher_loop_obj->clients = clients;
  int dispatch_server_baseport = pt.get<int>("dispatch.server_baseport");
  int dispatch_client_baseport = pt.get<int>("dispatch.client_baseport");
  router = new cyclone_switch(zmq_context,
			      &pt,
			      me,
			      replicas,
			      clients,
			      pt.get<int>("machines.machines"),
			      dispatch_server_baseport,
			      dispatch_client_baseport,
			      false,
			      false);
  (*dispatcher_loop_obj)();
}
