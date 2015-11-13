// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include "libcyclone.hpp"
#include "dispatcher_layout.hpp"
#include "../core/clock.hpp"
#include "cyclone_comm.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/log/utility/setup.hpp>
#include<boost/log/trivial.hpp>
#include <boost/thread.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include<libpmemobj.h>
#include "dispatcher_exec.hpp"
#include "timeouts.hpp"

static void *cyclone_handle;
static boost::property_tree::ptree pt;


static PMEMobjpool *state;
static rpc_callback_t execute_rpc;
static rpc_gc_callback_t gc_rpc;
static bool client_blocked[MAX_CLIENTS];
static int me;
static unsigned long last_global_txid;
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
    rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			  rpc->len - sizeof(rpc_t),
			  &rpc->ret_value);
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
	struct client_state_st *cstate = &D_RW(root)->client_state[client_id];
	pmemobj_tx_add_range_direct(cstate, sizeof(struct client_state_st));
	if(!TOID_IS_NULL(cstate->last_return_value)) {
	  TX_FREE(cstate->last_return_value);
	  
	}
	TOID_ASSIGN(cstate->last_return_value, OID_NULL);
	cstate->last_return_size = 0;
	__sync_synchronize();
	cstate->committed_txid = rpc->client_txid;
      }
    } TX_ONABORT{
      BOOST_LOG_TRIVIAL(fatal) << "Dispatcher tx abort !\n";
      exit(-1);
    } TX_END
  }
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}

int dispatcher_me()
{
  return me;
}

static unsigned char tx_buffer[DISP_MAX_MSGSIZE];
static unsigned char rx_buffer[DISP_MAX_MSGSIZE];
static cyclone_switch *router;

static void gc_pending_rpc_list()
{
  rpc_info_t *rpc, *deleted, *tmp;
  deleted = NULL;
  lock_rpc_list();
  rpc = pending_rpc_head;
  while(rpc != NULL) {
    if(rpc->complete) {
      tmp = rpc;
      rpc = rpc->next;
      tmp->next = deleted;
      deleted = tmp;
    }
    else {
      break;
    }
  }
  pending_rpc_head = rpc;
  if(rpc == NULL) {
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
	rpc_rep->code = RPC_REP_INVSRV;
	rpc_rep->master = cyclone_get_leader(cyclone_handle);
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
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc_info = locate_rpc(rpc->global_txid);
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
}

// Note: node cannot become master while this function is in progress
void cyclone_rep_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  bool issue_it;
  rpc_info_t *match;
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
  rpc_info = locate_rpc(rpc->global_txid);
  if(rpc_info == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to locate failed replication RPC !";
    exit(-1);
  }
  rpc_info->rep_failed = true;
  __sync_synchronize();
}


struct dispatcher_loop {
  void *zmq_context;
  int clients;
  
  void handle_rpc(unsigned long sz)
  {
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    unsigned long last_committed;
    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_t *rpc_rep = (rpc_t *)tx_buffer;
    rpc_info_t *rpc_info;
    unsigned long rep_sz = 0;
    switch(rpc_req->code) {
    case RPC_REQ_FN:
      rpc_rep->client_id   = rpc_req->client_id;
      rpc_rep->client_txid = rpc_req->client_txid;
      // Initiate replication
      rpc_req->global_txid = (++last_global_txid);
      issue_rpc(rpc_req, sz);
      // TBD: Conditional on rpc being deterministic
      void *cookie = cyclone_add_entry(cyclone_handle, rpc_req, sz);
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
	lock_rpc_list();
	rpc_info = pending_rpc_head;
	while(rpc_info != NULL) {
	  if(rpc_info->rpc->client_id == rpc_req->client_id &&
	     rpc_info->rpc->client_txid == rpc_req->client_txid) {
	    break;
	  }
	  rpc_info = rpc_info->next;
	}
	if(rpc_info != NULL) {
	  if(rpc_info->complete) {
	    unlock_rpc_list();
	    rpc_rep->code = RPC_REP_COMPLETE;
	    const struct client_state_st * s =
	      &D_RO(root)->client_state[rpc_req->client_id];
	    rpc_rep->code = RPC_REP_COMPLETE;
	    if(s->last_return_size > 0) {
	      memcpy(&rpc_rep->payload,
		     (void *)D_RO(s->last_return_value),
		     s->last_return_size);
	      rep_sz += s->last_return_size;
	    }
	  }
	  else {
	    unlock_rpc_list();
	    rpc_rep->code = RPC_REP_PENDING;
	  }
	}
	else {
	  unlock_rpc_list();
	  last_tx_committed =
	    D_RO(root)->client_state[rpc_req->client_id].committed_txid;
	  if(last_tx_committed == rpc_req->client_txid) {
	    const struct client_state_st * s =
	      &D_RO(root)->client_state[rpc_req->client_id];
	    rpc_rep->code = RPC_REP_COMPLETE;
	    if(s->last_return_size > 0) {
	      memcpy(&rpc_rep->payload,
		     (void *)D_RO(s->last_return_value),
		     s->last_return_size);
	      rep_sz += s->last_return_size;
	    }
	  }
	  else {
	    rpc_rep->code = RPC_REP_REDO;
	  }
	}
	if(rpc_rep->code == RPC_REQ_STATUS_BLOCK &&
	   rpc_rep->code == RPC_REP_PENDING) {
	  rep_sz = 0;
	  client_blocked[rpc_req->client_id] = true;
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
	gc_pending_rpc_list();
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
		      rpc_nvheap_setup_callback_t nvheap_setup_callback)
{
  boost::property_tree::read_ini(config_path, pt);
  boost::log::keywords::auto_flush = true;
  // Load/Setup state
  std::string file_path = pt.get<std::string>("dispatch.filepath");
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
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  BOOST_LOG_TRIVIAL(info) << "committed global txid = " 
			  << D_RO(root)->committed_global_txid;
  for(int i=0;i<MAX_CLIENTS;i++) {
   seen_client_txid[i] = D_RO(root)->client_state[i].committed_txid;
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
				NULL);
  // Listen on port
  void *zmq_context = zmq_init(1);
  me = pt.get<int>("network.me");
  dispatcher_loop_obj    = new dispatcher_loop();
  dispatcher_loop_obj->zmq_context = zmq_context;
  dispatcher_loop_obj->clients = pt.get<int>("dispatch.clients");
  int dispatch_server_baseport = pt.get<int>("dispatch.server_baseport");
  int dispatch_client_baseport = pt.get<int>("dispatch.client_baseport");
  router = new cyclone_switch(zmq_context,
			      &pt,
			      me,
			      dispatcher_loop_obj->clients,
			      dispatch_server_baseport,
			      dispatch_client_baseport,
			      false,
			      false);
  (*dispatcher_loop_obj)();
}
