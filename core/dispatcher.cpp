// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include<raft.h>
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
#include "checkpoint.hpp"

static void *cyclone_handle;
static boost::property_tree::ptree pt_server;
static boost::property_tree::ptree pt_client;

struct client_ro_state_st {
  volatile unsigned long committed_txid;
  char * last_return_value;
  int last_return_size;
} client_ro_state [MAX_CLIENTS];


static unsigned char tx_buffer[DISP_MAX_MSGSIZE];
static unsigned char rx_buffer[DISP_MAX_MSGSIZE];
static unsigned char tx_async_buffer[DISP_MAX_MSGSIZE];
static server_switch *router;


static PMEMobjpool *state;
static rpc_callback_t execute_rpc;
static rpc_leader_callback_t execute_rpc_leader;
static rpc_follower_callback_t execute_rpc_follower;
static rpc_gc_callback_t gc_rpc;
static int me;
// Linked list of RPCs yet to be completed
static rpc_info_t * volatile pending_rpc_head;
static rpc_info_t * volatile pending_rpc_tail;

static volatile unsigned long list_lock   = 0;
static volatile unsigned long result_lock = 0;
static volatile unsigned long pending_locks[263];
static volatile bool building_image = false;

static void lock(volatile unsigned long *lockp)
{
  // TEST + TEST&SET
  do {
    while((*lockp) != 0);
  } while(!__sync_bool_compare_and_swap(lockp, 0, 1));
  __sync_synchronize();
}

static void unlock(volatile unsigned long *lockp)
{
  __sync_synchronize();
  __sync_bool_compare_and_swap(lockp, 1, 0);
  __sync_synchronize();
}

static void lock_rpc_list()
{
  lock(&list_lock);
}

static void unlock_rpc_list()
{
  unlock(&list_lock);
}


static void client_response(rpc_info_t *rpc, rpc_t *rpc_rep)
{
  lock(&rpc->pending_lock);
  if(rpc->client_blocked == -1) {
    unlock(&rpc->pending_lock);
    return;
  }
  rpc_rep->client_id   = rpc->rpc->client_id;
  rpc_rep->client_txid = rpc->rpc->client_txid;
  rpc_rep->channel_seq = rpc->rpc->channel_seq;
  int rep_sz = sizeof(rpc_t);
  if(rpc->rep_failed) {
    rpc_rep->code = RPC_REP_UNKNOWN;
  }
  else {
    rpc_rep->code = RPC_REP_COMPLETE;
    if(rpc->sz > 0) {
      memcpy(rpc_rep + 1,
	     (void *)rpc->ret_value,
	     rpc->sz);
      rep_sz += rpc->sz;
    }
  }
  router->lock_output_socket(rpc->client_blocked,
			     rpc->rpc->client_id);
  cyclone_tx(router->output_socket(rpc->client_blocked,
				   rpc->rpc->client_id), 
	     (unsigned char *)rpc_rep, 
	     rep_sz, 
	     "Dispatch reply");
  router->unlock_output_socket(rpc->client_blocked,
			       rpc->rpc->client_id);
  rpc->client_blocked = -1;
  unlock(&rpc->pending_lock);
}

static rpc_info_t * locate_rpc_internal(int raft_idx,
					int raft_term,
					bool keep_lock = false)
{
  rpc_info_t *rpc_info;
  rpc_info_t *hit = NULL;
  lock_rpc_list();
  rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(rpc_info->raft_idx == raft_idx &&
       rpc_info->raft_term == raft_term) {
      hit = rpc_info;
      break;
    } 
    rpc_info = rpc_info->next;
  }
  if(!keep_lock) {
    unlock_rpc_list(); 
  }
  return hit;
}

static rpc_info_t * locate_rpc_next_commit(bool keep_lock = false)
{
  rpc_info_t *rpc_info;
  rpc_info_t *hit = NULL;
  lock_rpc_list();
  rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(!rpc_info->rep_failed && !rpc_info->rep_success) {
      hit = rpc_info;
      break;
    } 
    rpc_info = rpc_info->next;
  }
  if(!keep_lock) {
    unlock_rpc_list(); 
  }
  return hit;
}

static void dump_active_list()
{
  rpc_info_t *rpc_info;
  rpc_info_t *hit = NULL;
  rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    BOOST_LOG_TRIVIAL(info) << "ACTIVE  "
			    << rpc_info->raft_idx << ":"
			    << rpc_info->raft_term;
    rpc_info = rpc_info->next;
  }
}

static int get_max_client_txid(int client_id)
{
  int max_client_txid = 0;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  lock_rpc_list();
  rpc_info_t *rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(!rpc_info->rep_failed) {
      if(rpc_info->rpc->client_id == client_id) {
	if(rpc_info->rpc->client_txid > max_client_txid) {
	  max_client_txid = rpc_info->rpc->client_txid;
	}
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


static void mark_client_pending(int client_txid,
				unsigned long channel_seq,
				int client_id,
				int mc)
{
  int max_client_txid = 0;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  lock_rpc_list();
  rpc_info_t *rpc_info = pending_rpc_head;
  while(rpc_info != NULL) {
    if(rpc_info->rpc->client_id == client_id &&
       rpc_info->rpc->client_txid == client_txid) {
      lock(&rpc_info->pending_lock);
      rpc_info->rpc->channel_seq = channel_seq;
      rpc_info->client_blocked = mc;
      unlock(&rpc_info->pending_lock);
    }
    rpc_info = rpc_info->next;
  }
  unlock_rpc_list();
}

// This function must be executed in the context of a tx
static void mark_done(const rpc_t *rpc,
		      const int raft_idx,
		      const int raft_term,
		      const void* ret_value,
		      const int ret_size)
{
  int client_id = rpc->client_id;
  lock(&result_lock);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  volatile int *c_raft_idx_p  = &D_RW(root)->applied_raft_idx;
  volatile int *c_raft_term_p = &D_RW(root)->applied_raft_term;
  pmemobj_tx_add_range_direct((const void *)c_raft_idx_p, sizeof(int));
  pmemobj_tx_add_range_direct((const void *)c_raft_term_p, sizeof(int));
  *c_raft_idx_p  = raft_idx;
  *c_raft_term_p = raft_term;
  struct client_state_st *cstate = &D_RW(root)->client_state[client_id];
  pmemobj_tx_add_range_direct(cstate, sizeof(struct client_state_st));
  if(!TOID_IS_NULL(cstate->last_return_value)) {
    TX_FREE(cstate->last_return_value);
  }
  if(ret_size > 0) {
    cstate->last_return_value = TX_ALLOC(char, ret_size);
    pmemobj_memcpy_persist(state, D_RW(cstate->last_return_value), ret_value, ret_size);
  }
  else {
    TOID_ASSIGN(cstate->last_return_value, OID_NULL);
  }
  cstate->last_return_size = ret_size;
  __sync_synchronize(); // Main thread can return this value now
  cstate->committed_txid = rpc->client_txid;
  unlock(&result_lock);
}

void exec_rpc_internal_synchronous(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  volatile bool user_tx_aborted = false;
  volatile bool repeat = true;
  volatile int execution_term;
  volatile bool is_leader;
  volatile bool have_data;
  while(!rpc->rep_success && !rpc->rep_failed);
  if(rpc->rep_success) {
    while(repeat) {
      execution_term = cyclone_get_term(cyclone_handle);
      is_leader = cyclone_is_leader(cyclone_handle);
      have_data = rpc->have_follower_data;
      user_tx_aborted = false;
      __sync_synchronize();
      if(cyclone_get_term(cyclone_handle) != execution_term ||
	 cyclone_get_leader(cyclone_handle) == -1) {
	continue;
      }
      repeat = false;
      TX_BEGIN(state) {
	unsigned char *tmp;
	rpc_t *follower_data;
	int follower_data_size;
	if(is_leader && !have_data) {
	  rpc->sz = execute_rpc_leader((const unsigned char *)(rpc->rpc + 1),
				       rpc->len - sizeof(rpc_t),
				       &tmp,
				       &follower_data_size,
				       &rpc->ret_value);
	  follower_data = (rpc_t *)malloc(sizeof(rpc_t) + follower_data_size);
	  memcpy(follower_data + 1, tmp, follower_data_size);
	  follower_data->code = RPC_REQ_DATA;
	  follower_data->parent_raft_idx = rpc->raft_idx;
	  follower_data->parent_raft_term = rpc->raft_term;
	  follower_data->timestamp = rpc->rpc->timestamp;
	  rpc->req_follower_data = (char *)follower_data;
	  rpc->req_follower_term = execution_term;
	  rpc->req_follower_data_size = follower_data_size + sizeof(rpc_t);
	  gc_rpc(tmp);
	  __sync_synchronize();
	  rpc->req_follower_data_active = true;
	  __sync_synchronize();
	  while(rpc->req_follower_data_active);
	  free(follower_data);
	  while(!rpc->rep_follower_success &&
		cyclone_get_term(cyclone_handle) == execution_term);
	  if(cyclone_get_term(cyclone_handle) != execution_term) {
	    repeat = true;
	    pmemobj_tx_abort(-1);
	  }
	}
	else {
	  while(!rpc->rep_follower_success &&
		cyclone_get_term(cyclone_handle) == execution_term);
	  if(!rpc->rep_follower_success) {
	    repeat = true;
	    pmemobj_tx_abort(-1);
	  }
	  __sync_synchronize();
	  rpc->sz = execute_rpc_follower((const unsigned char *)(rpc->rpc + 1),
					 rpc->len - sizeof(rpc_t),
					 (unsigned char *)rpc->follower_data,
					 rpc->follower_data_size,
					 &rpc->ret_value);
	}
	mark_done(rpc->rpc, rpc->raft_idx, rpc->raft_term, rpc->ret_value, rpc->sz);
      } TX_ONABORT {
	if(!repeat) {
	  user_tx_aborted= true;
	}
      } TX_END
    }
  }
  if(user_tx_aborted) { // cleanup
    rpc->sz = 0;
    TX_BEGIN(state) {
      mark_done(rpc->rpc, rpc->raft_idx, rpc->raft_term, rpc->ret_value, rpc->sz);
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) << "Dispatcher tx abort !\n";
      exit(-1);
    } TX_END
  }
  client_response(rpc, (rpc_t *)tx_async_buffer);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}

void exec_rpc_internal(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  volatile bool user_tx_aborted = true;
  
  TX_BEGIN(state) {
    if(rpc->rpc->code == RPC_REQ_NODEADD) {
      rpc->sz= 0;
    }
    else if(rpc->rpc->code == RPC_REQ_NODEDEL) {
      rpc->sz = 0;
    }
    else {
      rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			    rpc->len - sizeof(rpc_t),
			    &rpc->ret_value);
    }
    while(!rpc->rep_success && !rpc->rep_failed);
    user_tx_aborted = false;
    if(rpc->rep_success) {
      mark_done(rpc->rpc, rpc->raft_idx, rpc->raft_term, rpc->ret_value, rpc->sz);
    }
    else {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    // Wait for replication to finish
    while(!rpc->rep_success && !rpc->rep_failed);
    if(rpc->rep_failed) {
      user_tx_aborted= false;
    }
  } TX_END
  if(user_tx_aborted) { // cleanup
    rpc->sz = 0;
    TX_BEGIN(state) {
      mark_done(rpc->rpc, rpc->raft_idx, rpc->raft_term, rpc->ret_value, rpc->sz);
    } TX_ONABORT{
      BOOST_LOG_TRIVIAL(fatal) << "Dispatcher tx abort !\n";
      exit(-1);
    } TX_END
  }
  client_response(rpc, (rpc_t *)tx_async_buffer);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}


void exec_rpc_internal_ro(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			rpc->len - sizeof(rpc_t),
			&rpc->ret_value);
  rpc->rep_success = true; // No replication needed
  struct client_ro_state_st *cstate = &client_ro_state[rpc->rpc->client_id];
  lock(&result_lock);
  if(cstate->last_return_size != 0) {
    free(cstate->last_return_value);
    cstate->last_return_size = 0;
  }
  if(rpc->sz > 0) {
    cstate->last_return_value = (char *)malloc(rpc->sz);
    memcpy(cstate->last_return_value,
	   rpc->ret_value,
	   rpc->sz);
    cstate->last_return_size = rpc->sz;
  }
  cstate->committed_txid = rpc->rpc->client_txid;
  unlock(&result_lock);
  client_response(rpc, (rpc_t *)tx_async_buffer);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}


static bool is_pending_rpc_list()
{
  rpc_info_t *tmp;
  tmp = pending_rpc_head;
  while(tmp) {
    if(!tmp->complete) {
      return true;
    }
    tmp = tmp->next;
  }
  return false;
}

static void gc_pending_rpc_list(bool is_master)
{
  rpc_info_t *rpc, *deleted, *tmp;
  void *cookie;
  deleted = NULL;
  tmp = pending_rpc_head;
  while(tmp) {
    if(tmp->req_follower_data_active) {
      cyclone_add_entry_term(cyclone_handle,
			     tmp->req_follower_data,
			     tmp->req_follower_data_size,
			     tmp->req_follower_term);
      __sync_synchronize();
      tmp->req_follower_data_active = false;
    }
    tmp = tmp->next;
  }
  lock_rpc_list();
  while(pending_rpc_head != NULL) {
    if(!pending_rpc_head->complete) {
      break;
    }
    tmp = pending_rpc_head;
    pending_rpc_head = pending_rpc_head->next;
    tmp->next = deleted;
    deleted = tmp;
  }
  if(pending_rpc_head == NULL) {
    pending_rpc_tail = NULL;
  }
  unlock_rpc_list();
  rpc_t *rpc_rep = (rpc_t *)tx_buffer;
  while(deleted) {
    tmp = deleted;
    deleted = deleted->next;    
    client_response(tmp, rpc_rep);
    if(tmp->sz != 0) {
      gc_rpc(tmp->ret_value);
    }
    if(tmp->follower_data != NULL) {
      free(tmp->follower_data);
    }
    delete tmp->rpc;
    delete tmp;
  }
}

static void issue_rpc(const rpc_t *rpc,
		      int len,
		      int raft_idx,
		      int raft_term)
{
  rpc_info_t *rpc_info = new rpc_info_t;
  rpc_info->raft_idx    = raft_idx;
  rpc_info->raft_term   = raft_term;
  rpc_info->rep_success = false;
  rpc_info->rep_failed  = false;
  rpc_info->complete    = false;
  rpc_info->len = len;
  rpc_info->rpc = (rpc_t *)(new char[len]);
  memcpy(rpc_info->rpc, rpc, len);
  rpc_info->rep_follower_success = false;
  rpc_info->follower_data = NULL;
  rpc_info->follower_data_size = 0;
  rpc_info->have_follower_data = false;
  rpc_info->req_follower_data_active = false;
  rpc_info->pending_lock = 0;
  rpc_info->client_blocked = -1;
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

void cyclone_commit_cb(void *user_arg, const unsigned char *data, const int len)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *rpc_info;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  int applied_raft_idx  = D_RO(root)->applied_raft_idx;
  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  else if(rpc->code == RPC_REQ_DATA) {
    if(rpc->parent_raft_idx  <= applied_raft_idx) {
      return;
    }
    rpc_info = locate_rpc_internal(rpc->parent_raft_idx,
				   rpc->parent_raft_term,
				   false);
    if(rpc_info == NULL) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to locate synchronous RPC for follower data completion: "
	<< rpc->parent_raft_idx << ":"
	<< rpc->parent_raft_term; 
      dump_active_list();
      exit(-1);
    }
    rpc_info->rep_follower_success = true;
    return;
  }
  rpc_info = locate_rpc_next_commit(true);
  if(rpc_info == NULL && rpc_info->raft_idx > applied_raft_idx) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Unable to locate any replicated RPC for commit";
    dump_active_list();
    exit(-1);
  }
  else if(rpc_info != NULL) {
    rpc_info->rep_success = true;
  }
  __sync_synchronize();
  unlock_rpc_list();
}

// Note: node cannot become master while this function is in progress
void cyclone_rep_cb(void *user_arg,
		    const unsigned char *data,
		    const int len,
		    const int raft_idx,
		    const int raft_term)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *match;
  TOID(disp_state_t) root;
  int applied_raft_idx;

  if(!building_image) {
    root = POBJ_ROOT(state, disp_state_t);
    applied_raft_idx = D_RO(root)->applied_raft_idx;
  }

  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  else if(rpc->code== RPC_REQ_DATA) {
    if(!building_image && rpc->parent_raft_idx  <= applied_raft_idx) {
      return;
    }
    match = locate_rpc_internal(rpc->parent_raft_idx,
				rpc->parent_raft_term,
				false);
    if(match == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Follower data rep couldn't locate RPC :"
			       << rpc->parent_raft_idx << ":"
			       << rpc->parent_raft_term;
      dump_active_list();
      exit(-1);
    }
    int fsize = len - sizeof(rpc_t);
    if(fsize > 0) {
      match->follower_data = malloc(fsize);
      memcpy(match->follower_data, rpc + 1, fsize);
    }
    match->follower_data_size = fsize;
    __sync_synchronize();
    match->have_follower_data = true;
    return;
  }
  if(!building_image && raft_idx  <= applied_raft_idx) {
    return;
  }
  issue_rpc(rpc, len, raft_idx, raft_term);
}

// Note: cyclone pop_cb cannot be called once the node becomes a master
void cyclone_pop_cb(void *user_arg,
		    const unsigned char *data,
		    const int len,
		    const int raft_idx,
		    const int raft_term)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *rpc_info;
  if(rpc->code == RPC_REQ_MARKER) {
    return;
  }
  if(rpc->code == RPC_REQ_DATA) {
    rpc_info = locate_rpc_internal(rpc->parent_raft_idx,
				   rpc->parent_raft_term,
				   true);
    rpc_info->have_follower_data = false;
    __sync_synchronize();
    rpc_info->follower_data_size = 0;
    if(rpc_info->follower_data != NULL) {
      free(rpc_info->follower_data);
      rpc_info->follower_data = NULL;
    }
    unlock_rpc_list();
    return;
  }
  rpc_info = locate_rpc_internal(raft_idx, raft_term, true);
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
  int machines;

  void send_kicker()
  {
    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_req->code = RPC_REQ_MARKER;
    void *cookie = cyclone_add_entry(cyclone_handle, rpc_req, sizeof(rpc_t));
    if(cookie != NULL) {
      free(cookie);
    }
  }

  void determine_status(rpc_t * rpc_req, rpc_t *rpc_rep, unsigned long* rep_sz)
  {
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    if(get_max_client_txid(rpc_req->client_id) < rpc_req->client_txid) {
      rpc_rep->code = RPC_REP_UNKNOWN;
      return;
    }
    else {
      lock(&result_lock);
      int last_rw_txid = D_RO(root)->client_state[rpc_req->client_id].committed_txid;
      int last_ro_txid = client_ro_state[rpc_req->client_id].committed_txid;
      if(((rpc_req->flags & RPC_FLAG_RO) == 0) &&
	 last_rw_txid < rpc_req->client_txid) {
	unlock(&result_lock);
	rpc_rep->code = RPC_REP_PENDING;
	return;
      }
      else if(((rpc_req->flags & RPC_FLAG_RO) != 0) &&
	      last_ro_txid < rpc_req->client_txid ) {
	unlock(&result_lock);
	rpc_rep->code = RPC_REP_PENDING;
	return;
      }
      else if(last_rw_txid == rpc_req->client_txid) {
	const struct client_state_st * s =
	  &D_RO(root)->client_state[rpc_req->client_id];
	*rep_sz = *rep_sz + s->last_return_size;
	if(s->last_return_size > 0) {
	  memcpy(rpc_rep + 1,
		 (void *)D_RO(s->last_return_value),
		 s->last_return_size);
	}
	unlock(&result_lock);
	rpc_rep->code = RPC_REP_COMPLETE;
	return;
      }
      else if(last_ro_txid == rpc_req->client_txid) {
	*rep_sz = *rep_sz + client_ro_state[rpc_req->client_id].last_return_size;
	if(client_ro_state[rpc_req->client_id].last_return_size > 0) {
	      memcpy(rpc_rep + 1,
		     client_ro_state[rpc_req->client_id].last_return_value,
		     client_ro_state[rpc_req->client_id].last_return_size);
	}
	unlock(&result_lock);
	rpc_rep->code = RPC_REP_COMPLETE;
	return;
      }
      else {
      	unlock(&result_lock);
	rpc_rep->code = RPC_REP_OLD;
      	return;
      }
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
    int requestor = rpc_req->requestor;
    rpc_rep->client_id   = rpc_req->client_id;
    rpc_rep->channel_seq = rpc_req->channel_seq;
    rpc_rep->client_txid = rpc_req->client_txid;
    rep_sz = sizeof(rpc_t);

    if(!cyclone_is_leader(cyclone_handle)) {
      rpc_rep->code = RPC_REP_INVSRV;
      rpc_rep->master = cyclone_get_leader(cyclone_handle);
    }
    else if(rpc_req->code == RPC_REQ_LAST_TXID) {
      rpc_rep->code = RPC_REP_COMPLETE;
      rpc_rep->last_client_txid = get_max_client_txid(rpc_req->client_id);
    }
    else {
      determine_status(rpc_req, rpc_rep, &rep_sz);
      // Issue if necessary
      if(rpc_rep->code == RPC_REP_UNKNOWN  &&  rpc_req->code != RPC_REQ_STATUS) {
	if(rpc_req->flags & RPC_FLAG_RO) {
	  // Distinguish ro txids from rw txids
	  issue_rpc(rpc_req, sz, -1, -1);
	  rpc_rep->code = RPC_REP_PENDING;
	}
	else {
	  // Initiate replication
	  if(rpc_req->code == RPC_REQ_FN) {
	    cookie = cyclone_add_entry(cyclone_handle, rpc_req, sz);
	    rpc_rep->code = RPC_REP_PENDING;
	  }
	  else if(rpc_req->code == RPC_REQ_NODEADD) {
	    // Wait for pipeline to drain
	    while(is_pending_rpc_list());
	    int chosen_raft_idx, chosen_raft_term;
	    do {
	      chosen_raft_term = D_RO(root)->applied_raft_term; 
	      __sync_synchronize();
	      chosen_raft_idx  = D_RO(root)->applied_raft_idx;
	    } while(chosen_raft_term != D_RO(root)->applied_raft_term);
	    cfg_change_t *cfg = (cfg_change_t *)(rpc_req + 1);
	    cfg->last_included_term = chosen_raft_term;
	    cfg->last_included_idx = chosen_raft_idx;
	    take_checkpoint(cyclone_get_term(cyclone_handle),
			    chosen_raft_idx,
			    chosen_raft_term);
	    cookie = cyclone_add_entry_cfg(cyclone_handle,
					   RAFT_LOGTYPE_ADD_NONVOTING_NODE,
					   rpc_req,
					   sz);
	    // Async send checkpoint 
	    exec_send_checkpoint(cyclone_control_socket_out(cyclone_handle, 
							    cfg->node),
				 cyclone_handle);
	    rpc_rep->code = RPC_REP_COMPLETE;
	  }
	  else {
	    cookie = cyclone_add_entry_cfg(cyclone_handle,
					   RAFT_LOGTYPE_REMOVE_NODE,
					   rpc_req,
					   sz);
	    rpc_rep->code = RPC_REP_COMPLETE;
	  }
	  if(cookie != NULL) {
	    free(cookie);
	  }
	  else {
	    rpc_rep->code = RPC_REP_INVSRV;
	    rpc_rep->master = cyclone_get_leader(cyclone_handle);
	  }
	}
      }
    }
    if(rpc_rep->code == RPC_REP_PENDING) {
      mark_client_pending(rpc_req->client_txid,
			  rpc_req->channel_seq,
			  rpc_req->client_id,
			  requestor);
      rep_sz = 0;
    }
    if(rep_sz > 0) {
      router->lock_output_socket(requestor, rpc_req->client_id);
      cyclone_tx(router->output_socket(requestor, rpc_req->client_id), 
		 tx_buffer, 
		 rep_sz, 
		 "Dispatch reply");
      router->unlock_output_socket(requestor, rpc_req->client_id);
    }
  }

  void operator ()()
  {
    rtc_clock clock;
    bool is_master = false;
    unsigned long last_gc = clock.current_time();
    while(building_image); // Wait till building image is complete
    while(true) {
      unsigned long sz = cyclone_rx_noblock(router->input_socket(),
					    rx_buffer,
					    DISP_MAX_MSGSIZE,
					    "DISP RCV");
      if(sz != -1) {
	handle_rpc(sz);
      }
      if((clock.current_time() - last_gc) >= PERIODICITY) {
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
	last_gc = clock.current_time();
      }
    }
  }
};

static dispatcher_loop * dispatcher_loop_obj;
static rpc_nvheap_setup_callback_t nvheap_setup_callback_saved;


void checkpoint_callback(void *socket)
{
  build_image(socket);
  state = pmemobj_open(get_checkpoint_fname(), "disp_state");
  if(state == NULL) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Unable to open pmemobj pool "
      << get_checkpoint_fname()
      << " for dispatcher state:"
      << strerror(errno);
    exit(-1);
  }
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  TX_BEGIN(state) {
    D_RW(root)->nvheap_root = 
      nvheap_setup_callback_saved(D_RO(root)->nvheap_root, state);
  } TX_ONABORT {
    BOOST_LOG_TRIVIAL(fatal)
      << "Application unable to recover state:"
      << strerror(errno);
    exit(-1);
  } TX_END
  BOOST_LOG_TRIVIAL(info) << "DISPATCHER: Recovered from checkpoint";
  building_image = false;
}

void dispatcher_start(const char* config_server_path,
		      const char* config_client_path,
		      rpc_callback_t rpc_callback,
		      rpc_leader_callback_t rpc_leader_callback,
		      rpc_follower_callback_t rpc_follower_callback,
		      rpc_gc_callback_t gc_callback,
		      rpc_nvheap_setup_callback_t nvheap_setup_callback,
		      int me,
		      int replicas,
		      int clients)
{
  boost::property_tree::read_ini(config_server_path, pt_server);
  boost::property_tree::read_ini(config_client_path, pt_client);
  // Load/Setup state
  std::string file_path = pt_server.get<std::string>("dispatch.filepath");
  char me_str[100];
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  init_checkpoint(file_path.c_str(), me);
  nvheap_setup_callback_saved = nvheap_setup_callback;
  dispatcher_exec_startup();

  bool i_am_active = false;
  for(int i=0;i<pt_server.get<int>("active.replicas");i++) {
    char nodeidxkey[100];
    sprintf(nodeidxkey, "active.entry%d",i);
    int nodeidx = pt_server.get<int>(nodeidxkey);
    if(nodeidx == me) {
      i_am_active = true;
    }
  }

  if(!i_am_active) {
    BOOST_LOG_TRIVIAL(info) << "Starting inactive server";
    building_image = true;
  }
  else {
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
	D_RW(root)->nvheap_root = nvheap_setup_callback(TOID_NULL(char), state);
	D_RW(root)->applied_raft_idx = -1;
	D_RW(root)->applied_raft_term  = -1;
      } TX_ONABORT {
	BOOST_LOG_TRIVIAL(fatal) 
	  << "Unable to setup dispatcher state:"
	  << strerror(errno);
	exit(-1);
      } TX_END
    }
    else {
      state = pmemobj_open(file_path.c_str(), "disp_state");
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
  }
  // Setup RO state
  for(int i = 0;i < MAX_CLIENTS;i++) {
    client_ro_state[i].committed_txid    = 0UL;
    client_ro_state[i].last_return_size  = 0;
    client_ro_state[i].last_return_value = NULL;
  }
  
  execute_rpc = rpc_callback;
  execute_rpc_follower = rpc_follower_callback;
  execute_rpc_leader = rpc_leader_callback;

  gc_rpc      = gc_callback;
  pending_rpc_head = pending_rpc_tail = NULL;
  // Boot cyclone -- this can lead to rep cbs on recovery
  cyclone_handle = cyclone_boot(config_server_path,
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_commit_cb,
				&checkpoint_callback,
				me,
				replicas,
				NULL);
  // Listen on port
  void *zmq_context = zmq_init(1);
  dispatcher_loop_obj    = new dispatcher_loop();
  dispatcher_loop_obj->zmq_context = zmq_context;
  dispatcher_loop_obj->clients  = clients;
  dispatcher_loop_obj->machines = pt_client.get<int>("machines.machines");
  router = new server_switch(zmq_context,
			     &pt_server,
			     &pt_client,
			     me,
			     clients,
			     false);
  for(int i=0;i<263;i++) {
    pending_locks[i] = 0;
  }
  (*dispatcher_loop_obj)();
}
