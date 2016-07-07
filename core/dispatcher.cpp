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
#include "tuning.hpp"
#include "checkpoint.hpp"

static void *cyclone_handle;
static boost::property_tree::ptree pt_server;
static boost::property_tree::ptree pt_client;

#if defined(DPDK_STACK)
void * global_dpdk_context;
#endif


struct client_ro_state_st {
  volatile unsigned long committed_txid;
  char * last_return_value;
  int last_return_size;
  bool inflight;
} client_ro_state [MAX_CLIENTS];

static unsigned long ro_result_lock = 0;

static unsigned char tx_buffer[DISP_MAX_MSGSIZE];
static unsigned char *rx_buffer;
static unsigned char *rx_buffers;
static unsigned char tx_async_buffer[DISP_MAX_MSGSIZE];
static server_switch *router;
static int replica_me;

static PMEMobjpool *state;
static rpc_callbacks_t app_callbacks;


static int me;
// Linked list of RPCs yet to be completed
static rpc_info_t * volatile pending_rpc_head;
static rpc_info_t * volatile pending_rpc_tail;
static volatile int committed_raft_log_idx;

static volatile unsigned long list_lock   = 0;
static volatile bool building_image = false;
static void* follower_req_socket = NULL;
static void* follower_rep_socket = NULL;

static void lock_rpc_list()
{
  lock(&list_lock);
}

static void unlock_rpc_list()
{
  unlock(&list_lock);
}


static void client_response(rpc_info_t *rpc, 
			    rpc_t *rpc_rep,
			    int mux_index)
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
  router->send_data(rpc->client_blocked,
		    rpc->rpc->client_id,
		    (void *)rpc_rep, 
		    rep_sz,
		    mux_index); 
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
       rpc_info->raft_term == raft_term &&
       !rpc_info->rep_failed) {
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
			    << rpc_info->raft_term 
			    << " success = "
			    << rpc_info->rep_success
			    << " failed = "
			    << rpc_info->rep_failed;
    rpc_info = rpc_info->next;
  }
}

static int get_max_client_txid(int client_id)
{
  int max_client_txid = 0;
  rpc_cookie_t cookie;
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
  cookie.client_id = client_id;
  app_callbacks.cookie_lock_callback(&cookie);
  app_callbacks.cookie_unlock_callback();
  int last_rw_txid = cookie.client_txid;
  lock(&ro_result_lock);
  int last_ro_txid = client_ro_state[client_id].committed_txid;
  unlock(&ro_result_lock);
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

void init_rpc_cookie_info(rpc_cookie_t *cookie, rpc_info_t *rpc)
{
  cookie->raft_idx  = rpc->raft_idx;
  cookie->raft_term = rpc->raft_term;
  cookie->client_id = rpc->rpc->client_id;
  cookie->client_txid = rpc->rpc->client_txid;
}

void exec_rpc_internal_synchronous(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  volatile bool repeat = true;
  volatile int execution_term;
  volatile bool is_leader;
  volatile bool have_data;
  rpc_cookie_t rpc_cookie;
  follower_req_t follower_req;
  int follower_resp;
  void *tx_handle;
  init_rpc_cookie_info(&rpc_cookie, rpc);
  while(!rpc->rep_success && !rpc->rep_failed);
  if(rpc->rep_success) {
    while(repeat) {
      execution_term = cyclone_get_term(cyclone_handle); // get current view
      if(cyclone_get_leader(cyclone_handle) == -1) { // Make sure its stable
	continue;
      }
      is_leader = cyclone_is_leader(cyclone_handle);
      have_data = rpc->have_follower_data;
      __sync_synchronize();
      if(cyclone_get_term(cyclone_handle) != execution_term) {
	continue; // Make sure view hasn't changed
      }
      repeat = false;
      unsigned char *tmp;
      rpc_t *follower_data;
      int follower_data_size;
      if(is_leader && !have_data) {
	tx_handle = app_callbacks.rpc_leader_callback((const unsigned char *)(rpc->rpc + 1),
						      rpc->len - sizeof(rpc_t),
						      &tmp,
						      &follower_data_size,
						      &rpc_cookie);
	rpc->ret_value = rpc_cookie.ret_value;
	rpc->sz  = rpc_cookie.ret_size;
	follower_data = (rpc_t *)malloc(sizeof(rpc_t) + follower_data_size);
	memcpy(follower_data + 1, tmp, follower_data_size);
	follower_data->code = RPC_REQ_DATA;
	follower_data->parent_raft_idx = rpc->raft_idx;
	follower_data->parent_raft_term = rpc->raft_term;
	follower_data->timestamp = rpc->rpc->timestamp;
	follower_req.req_follower_data = (char *)follower_data;
	follower_req.req_follower_term = execution_term;
	follower_req.req_follower_data_size = follower_data_size + sizeof(rpc_t);
	cyclone_tx_loopback_block(follower_req_socket, 
				  (const unsigned char *)&follower_req,
				  sizeof(follower_req_t),
				  "follower req");
	cyclone_rx_loopback_block(follower_req_socket,
				  (unsigned char *)&follower_resp,
				  sizeof(int),
				  "follower req resp");
	app_callbacks.gc_callback(tmp);
	free(follower_data);
	while(!rpc->rep_follower_success &&
	      cyclone_get_term(cyclone_handle) == execution_term);
	if(!rpc->rep_follower_success) {
	  repeat = true;
	  app_callbacks.tx_abort(tx_handle);
	}
      }
      else {
	while(!rpc->rep_follower_success &&
	      cyclone_get_term(cyclone_handle) == execution_term);
	if(!rpc->rep_follower_success) {
	  repeat = true;
	} 
	else {
	  __sync_synchronize();
	  app_callbacks.rpc_follower_callback((const unsigned char *)(rpc->rpc + 1),
					      rpc->len - sizeof(rpc_t),
					      (unsigned char *)rpc->follower_data,
					      rpc->follower_data_size,
					      &rpc_cookie);
	
	  rpc->sz    = rpc_cookie.ret_size;
	  rpc->ret_value = rpc_cookie.ret_value; 
	}
      }
      if(!repeat) {
	app_callbacks.tx_commit(tx_handle, &rpc_cookie);
      }
      else {
	if(rpc->sz > 0) {
	  app_callbacks.gc_callback(rpc->ret_value);
	  rpc->sz = 0;
	  rpc->ret_value = NULL;
	}
      }
    }
  }
  client_response(rpc, (rpc_t *)tx_async_buffer, 1);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}

void exec_rpc_internal(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc_cookie_t rpc_cookie;
  void *tx_handle;
  init_rpc_cookie_info(&rpc_cookie, rpc);
  if(rpc->rpc->code == RPC_REQ_NODEADD) {
    rpc->sz= 0;
  }
  else if(rpc->rpc->code == RPC_REQ_NODEDEL) {
    rpc->sz = 0;
  }
  else {
    tx_handle = app_callbacks.rpc_callback((const unsigned char *)(rpc->rpc + 1),
					   rpc->len - sizeof(rpc_t),
					   &rpc_cookie);
    rpc->sz = rpc_cookie.ret_size;
    rpc->ret_value = rpc_cookie.ret_value;
  }
  while(!rpc->rep_success && !rpc->rep_failed);
  if(rpc->rep_success) {
    app_callbacks.tx_commit(tx_handle, &rpc_cookie);
  }
  else {
    app_callbacks.tx_abort(tx_handle);
  } 
  client_response(rpc, (rpc_t *)tx_async_buffer, 1);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}

void exec_rpc_internal_seq(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc_cookie_t rpc_cookie;
  void *tx_handle;
  init_rpc_cookie_info(&rpc_cookie, rpc);
  while(!rpc->rep_success && !rpc->rep_failed);
  if(rpc->rpc->code == RPC_REQ_NODEADD) {
    rpc->sz= 0;
  }
  else if(rpc->rpc->code == RPC_REQ_NODEDEL) {
    rpc->sz = 0;
  }
  else {
    tx_handle = app_callbacks.rpc_callback((const unsigned char *)(rpc->rpc + 1),
					   rpc->len - sizeof(rpc_t),
					   &rpc_cookie);
    rpc->sz = rpc_cookie.ret_size;
    rpc->ret_value = rpc_cookie.ret_value;
  }
  if(rpc->rep_success) {
    app_callbacks.tx_commit(tx_handle, &rpc_cookie);
  }
  else {
    app_callbacks.tx_abort(tx_handle);
  } 
  client_response(rpc, (rpc_t *)tx_async_buffer, 1);
  __sync_synchronize();
  rpc->complete = true; // note: rpc will be freed after this
}


void exec_rpc_internal_ro(rpc_info_t *rpc)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  rpc_cookie_t rpc_cookie;
  init_rpc_cookie_info(&rpc_cookie, rpc);
  app_callbacks.rpc_callback((const unsigned char *)(rpc->rpc + 1),
			     rpc->len - sizeof(rpc_t),
			     &rpc_cookie);
  rpc->ret_value = rpc_cookie.ret_value;
  rpc->sz  = rpc_cookie.ret_size;
  rpc->rep_success = true; // No replication needed
  struct client_ro_state_st *cstate = &client_ro_state[rpc->rpc->client_id];
  lock(&ro_result_lock);
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
  unlock(&ro_result_lock);
  client_response(rpc, (rpc_t *)tx_async_buffer, 1);
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
    client_response(tmp, rpc_rep, 0);
    if(tmp->sz != 0) {
      app_callbacks.gc_callback(tmp->ret_value);
    }
    if(tmp->follower_data != NULL) {
      free(tmp->follower_data);
    }
    free(tmp->rpc);
    free(tmp);
  }
}

static void issue_rpc(const rpc_t *rpc,
		      int len,
		      int raft_idx,
		      int raft_term,
		      bool mark_resp)
{
  rpc_info_t *rpc_info = (rpc_info_t *)malloc(sizeof(rpc_info_t));
  rpc_info->raft_idx    = raft_idx;
  rpc_info->raft_term   = raft_term;
  rpc_info->rep_success = false;
  rpc_info->rep_failed  = false;
  rpc_info->complete    = false;
  rpc_info->len = len;
  rpc_info->rpc = (rpc_t *)malloc(len);
  memcpy(rpc_info->rpc, rpc, len);
  rpc_info->rep_follower_success = false;
  rpc_info->follower_data = NULL;
  rpc_info->follower_data_size = 0;
  rpc_info->have_follower_data = false;
  rpc_info->pending_lock = 0;
  if(mark_resp) {
    rpc_info->client_blocked = rpc_info->rpc->requestor;
  }
  else {
    rpc_info->client_blocked = -1;
  }
  rpc_info->next = NULL;
  rpc_info->sz = 0;
  rpc_info->ret_value = NULL;
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

void cyclone_commit_cb(void *user_arg, 
		       const unsigned char *data, 
		       const int len,
		       const int raft_idx,
		       const int raft_term)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_info_t *rpc_info;
  rpc_cookie_t rpc_cookie;
  app_callbacks.cookie_get_callback(&rpc_cookie);
  int applied_raft_idx  = rpc_cookie.raft_idx;
  if(raft_idx  <= applied_raft_idx) {
    return;
  }
  if(committed_raft_log_idx >=  raft_idx) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Error in commit idx (cyclone_commit_cb) "
      << raft_idx 
      << ":"
      << raft_term;
    BOOST_LOG_TRIVIAL(fatal)
      << " Committed so far "
      << committed_raft_log_idx;
    dump_active_list();
    exit(-1);
  }
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
    __sync_synchronize();
    return;
  }
  rpc_info = locate_rpc_internal(raft_idx, raft_term, true);
  if(rpc_info == NULL) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Unable to locate any replicated RPC for commit "
      << raft_idx 
      << ":"
      << raft_term;
    dump_active_list();
    exit(-1);
  }
  else {
    rpc_info->rep_success = true;
  }
  __sync_synchronize();
  unlock_rpc_list();
  committed_raft_log_idx = raft_idx;
}

// Note: node cannot become master while this function is in progress
void cyclone_rep_cb(void *user_arg,
		    const unsigned char *data,
		    const int len,
		    const int raft_idx,
		    const int raft_term,
		    replicant_t *rep)
{
  const rpc_t *rpc = (const rpc_t *)data;
  rpc_t rpc_client_assist;
  rpc_info_t *match;
  int applied_raft_idx;

  if(committed_raft_log_idx >=  raft_idx) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Error in commit idx (cyclone_rep_cb) "
      << raft_idx 
      << ":"
      << raft_term;
    BOOST_LOG_TRIVIAL(fatal)
      << " Committed so far "
      << committed_raft_log_idx;
    dump_active_list();
    exit(-1);
  }

  if(!building_image) {
    rpc_cookie_t cookie;
    app_callbacks.cookie_get_callback(&cookie);
    applied_raft_idx = cookie.raft_idx;
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

  if(rep != NULL) {
    rpc_client_assist.client_id   = rpc->client_id;
    rpc_client_assist.client_txid = rpc->client_txid;
    rpc_client_assist.channel_seq = rpc->channel_seq;
    rpc_client_assist.code = RPC_REQ_ASSIST;
    rpc_client_assist.rep  = *rep;
    // Engage client assist
    router->send_data(rpc->requestor,
		      rpc->client_id,
		      &rpc_client_assist, 
		      sizeof(rpc_t),
		      2);
  }

  issue_rpc(rpc, len, raft_idx, raft_term, 
	    (rpc->receiver == replica_me ? true:false));
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
  if(rpc_info->rep_success) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Rolling back successful RPC ! (cyclone_pop_cb) "
      << raft_idx 
      << ":"
      << raft_term;
    dump_active_list();
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
  boost::thread *tx_thread;

  void send_kicker()
  {
    rpc_t rpc_req;
    rpc_req.code = RPC_REQ_MARKER;
    rpc_req.timestamp = rtc_clock::current_time();
    void *cookie = cyclone_add_entry(cyclone_handle, &rpc_req, sizeof(rpc_t));
    if(cookie != NULL) {
      free(cookie);
    }
  }

  void determine_status(rpc_t * rpc_req, rpc_t *rpc_rep, unsigned long* rep_sz)
  {
    rpc_cookie_t cookie;
    if(get_max_client_txid(rpc_req->client_id) < rpc_req->client_txid) {
      rpc_rep->code = RPC_REP_UNKNOWN;
      return;
    }
    else {
      cookie.client_id = rpc_req->client_id;
      app_callbacks.cookie_lock_callback(&cookie);
      lock(&ro_result_lock);
      int last_rw_txid = cookie.client_txid;
      int last_ro_txid = client_ro_state[rpc_req->client_id].committed_txid;
      if(((rpc_req->flags & RPC_FLAG_RO) == 0) &&
	 last_rw_txid < rpc_req->client_txid) {
	unlock(&ro_result_lock);
	app_callbacks.cookie_unlock_callback();
	rpc_rep->code = RPC_REP_PENDING;
	return;
      }
      else if(((rpc_req->flags & RPC_FLAG_RO) != 0) &&
	      last_ro_txid < rpc_req->client_txid ) {
	unlock(&ro_result_lock);
	app_callbacks.cookie_unlock_callback();
	rpc_rep->code = RPC_REP_PENDING;
	return;
      }
      else if(last_rw_txid == rpc_req->client_txid) {
	*rep_sz = *rep_sz + cookie.ret_size;
	if(cookie.ret_size > 0) {
	  memcpy(rpc_rep + 1, cookie.ret_value, cookie.ret_size);
	}
	unlock(&ro_result_lock);
	app_callbacks.cookie_unlock_callback();
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
	unlock(&ro_result_lock);
	app_callbacks.cookie_unlock_callback();
	rpc_rep->code = RPC_REP_COMPLETE;
	return;
      }
      else {
	unlock(&ro_result_lock);
	app_callbacks.cookie_unlock_callback();
	rpc_rep->code = RPC_REP_OLD;
      	return;
      }
    }
  }
        
  bool handle_rpc(int sz)
  {
    rpc_t *rpc_req = (rpc_t *)rx_buffer;
    rpc_t *rpc_rep = (rpc_t *)tx_buffer;
    rpc_info_t *rpc_info;
    unsigned long rep_sz = 0;
    void *cookie;
    unsigned long last_tx_committed;
    int requestor = rpc_req->requestor;
    bool will_issue_rpc = false;
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    rpc_rep->client_id   = rpc_req->client_id;
    rpc_rep->channel_seq = rpc_req->channel_seq;
    rpc_rep->client_txid = rpc_req->client_txid;
    rep_sz = sizeof(rpc_t);
    bool batch_issue = false;
    rpc_req->receiver = replica_me;
    client_ro_state[rpc_req->client_id].inflight = false;
    
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
      if(rpc_rep->code == RPC_REP_UNKNOWN  &&  rpc_req->code != RPC_REQ_STATUS)
      {
	will_issue_rpc = true;
	if(rpc_req->flags & RPC_FLAG_RO) {
	  // Distinguish ro txids from rw txids
	  issue_rpc(rpc_req, sz, -1, -1, true);
	  rpc_rep->code = RPC_REP_PENDING;
	}
	else {
	  // Initiate replication
	  if(rpc_req->code == RPC_REQ_FN) {
	    batch_issue = true;
	    rpc_rep->code = RPC_REP_PENDING;
	  }
	  else if(rpc_req->code == RPC_REQ_NODEADD) {
	    // Wait for pipeline to drain
	    while(is_pending_rpc_list());
	    int chosen_raft_idx, chosen_raft_term;
	    rpc_cookie_t rpc_cookie;
	    app_callbacks.cookie_get_callback(&rpc_cookie);
	    chosen_raft_term = rpc_cookie.raft_term; 
	    chosen_raft_idx  = rpc_cookie.raft_idx;
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
	  if(!batch_issue) {
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
    }
    if(rpc_rep->code == RPC_REP_PENDING) {
      if(!will_issue_rpc) {
	mark_client_pending(rpc_req->client_txid,
			    rpc_req->channel_seq,
			    rpc_req->client_id,
			    requestor);
      }
      rep_sz = 0;
    }
    if(rep_sz > 0) {
      router->send_data(requestor,
			rpc_req->client_id, 
			(void *)tx_buffer, 
			rep_sz,
			0); 
    }
    return batch_issue;
  }

  void batch_issue_rpc(unsigned char *ptr, int *sizes, int batch_size)
  {
    void *cookies = cyclone_add_batch(cyclone_handle,
				      ptr,
				      sizes,
				      batch_size);
    if(cookies == NULL) {
      for(int i=0;i<batch_size;i++) {
	rpc_t *rpc_req = (rpc_t *)ptr;
	rpc_t *rpc_rep = (rpc_t *)tx_buffer;
	rpc_rep->code = RPC_REP_INVSRV;
	rpc_rep->master = cyclone_get_leader(cyclone_handle);
	router->send_data(rpc_req->requestor,
			  rpc_req->client_id,
			  (void *)tx_buffer, 
			  sizeof(rpc_t),
			  0);
	ptr = ptr + sizes[i];
      }
    }
    else {
      free(cookies);
    }
  }

  bool *need_issue_rpc;

  void handle_batch_rpc(int *rx_sizes, int requests)
  {
    int i;
    rx_buffer = rx_buffers;
    for(int i=0; i < requests;i++) {
      need_issue_rpc[i] = handle_rpc(rx_sizes[i]);
      rx_buffer += rx_sizes[i];
    }
    rx_buffer = rx_buffers;
    int batch_head = -1;
    void *batch_head_ptr = NULL;
    for(i=0;i < requests;i++) {
      if(!need_issue_rpc [i] && batch_head != -1) {
	batch_issue_rpc((unsigned char *)batch_head_ptr, &rx_sizes[batch_head], i - batch_head);
	batch_head = -1;
	batch_head_ptr = NULL;
      }
      else if(need_issue_rpc[i] && batch_head == -1) {
	batch_head = i;
	batch_head_ptr = rx_buffer;
      }
      rx_buffer += rx_sizes[i];
    }
    if(batch_head != -1) {
      batch_issue_rpc((unsigned char *)batch_head_ptr, &rx_sizes[batch_head], i - batch_head);
    }
  }

  
  void operator ()()
  {
    bool is_master = false;
    unsigned long last_gc = rtc_clock::current_time();
    unsigned long last_batch = last_gc;
    int requests = 0;
    unsigned char * ptr;
    int *rx_sizes = (int *)malloc(MAX_BATCH_SIZE*sizeof(int));
    need_issue_rpc = (bool *)malloc(MAX_BATCH_SIZE*sizeof(bool));
    rpc_t *req;
    const int BUFSPACE = MIN_BATCH_BUFFERS*DISP_MAX_MSGSIZE;
    int adaptive_batch_size = MIN_BATCH_BUFFERS;
    int space_left = BUFSPACE;
    follower_req_t follower_req;
    int follower_resp;

    rx_buffers = (unsigned char *)malloc(BUFSPACE);
    
    while(building_image); // Wait till building image is complete
    ptr = rx_buffers;
    while(true) {
      int sz = cyclone_rx(router->input_socket(),
			  ptr,
			  DISP_MAX_MSGSIZE,
			  "DISP RCV");
      unsigned long mark = rtc_clock::current_time();
      if(sz != -1) {
	rx_sizes[requests] = sz;
	req = (rpc_t *)ptr;
	req->timestamp = mark;
	if(router->client_port(req->requestor, req->client_id) != req->client_port) {
	  router->ring_doorbell(req->requestor,
				req->client_id,
				req->client_port,
				0);
	}
	if(!client_ro_state[req->client_id].inflight) { // Only one inflight per client
	  ptr += sz;
	  space_left -= sz;
	  requests++;
	  if(requests == 1) {
	    last_batch = mark;
	  }
	  if(req->code == RPC_REQ_NODEADD || req->code == RPC_REQ_NODEDEL) {
	    if(requests > 1) {
	      handle_batch_rpc(rx_sizes, requests - 1);
	    }
	    memcpy(rx_buffers, ptr - sz, sz);
	    rx_sizes[0] = sz;
	    handle_batch_rpc(rx_sizes, 1);
	    requests = 0;
	    ptr = rx_buffers;
	    space_left = BUFSPACE;
	  }
	  else if(requests == adaptive_batch_size) {
	    handle_batch_rpc(rx_sizes, requests);
	    requests = 0;
	    ptr = rx_buffers;
	    space_left = BUFSPACE;
	    adaptive_batch_size = adaptive_batch_size*2;
	    if(adaptive_batch_size > MAX_BATCH_SIZE) {
	      adaptive_batch_size = MAX_BATCH_SIZE;
	    }
	  }
	  else if(space_left < DISP_MAX_MSGSIZE) {
	    handle_batch_rpc(rx_sizes, requests);
	    requests = 0;
	    ptr = rx_buffers;
	    space_left = BUFSPACE;
	    // Don't change adaptive batch size
	  }
	  else {
	    client_ro_state[req->client_id].inflight = true;
	  }
	}
      }
      else if(requests > 0 && 
	      (mark - last_batch) >= DISP_BATCHING_INTERVAL) {
	handle_batch_rpc(rx_sizes, requests);
	requests = 0;
	ptr = rx_buffers;
	space_left = BUFSPACE;
	if(adaptive_batch_size > 1) {
	  adaptive_batch_size = adaptive_batch_size/2;
	}
      }

      if((mark - last_gc) >= PERIODICITY) {
	sz = cyclone_rx_loopback(follower_rep_socket,
				 (unsigned char *)&follower_req,
				 sizeof(follower_req_t),
				 "follower req");
	if(sz != -1) {
	  void * cookie = 
	    cyclone_add_entry_term(cyclone_handle,
				   follower_req.req_follower_data,
				   follower_req.req_follower_data_size,
				   follower_req.req_follower_term);
	  if(cookie != NULL) {
	    free(cookie);
	  }
	  follower_resp = 0;
	  cyclone_tx_loopback_block(follower_rep_socket,
				    (unsigned char *)&follower_resp,
				    sizeof(int),
				    "follower rep rep");
	}
	
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
	last_gc = mark;
      }
    }
  }
};

static dispatcher_loop * dispatcher_loop_obj;


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
      app_callbacks.nvheap_setup_callback(D_RO(root)->nvheap_root, state);
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
		      rpc_callbacks_t *rpc_callbacks,
		      int me,
		      int replicas,
		      int clients)
{
  boost::property_tree::read_ini(config_server_path, pt_server);
  boost::property_tree::read_ini(config_client_path, pt_client);
  // Load/Setup state
  std::string file_path = pt_server.get<std::string>("dispatch.filepath");
  unsigned long heapsize = pt_server.get<unsigned long>("dispatch.heapsize");
  char me_str[100];
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  init_checkpoint(file_path.c_str(), me);
  app_callbacks = *rpc_callbacks;
  dispatcher_exec_startup();

  
#if defined(DPDK_STACK)
  global_dpdk_context = dpdk_context();
#endif

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
			     heapsize + PMEMOBJ_MIN_POOL,
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
	D_RW(root)->nvheap_root = app_callbacks.nvheap_setup_callback(TOID_NULL(char), state);
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
	D_RW(root)->nvheap_root = 
	  app_callbacks.nvheap_setup_callback(D_RO(root)->nvheap_root, state);
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
    client_ro_state[i].inflight = false;
  }

  committed_raft_log_idx = -1;

  pending_rpc_head = pending_rpc_tail = NULL;
  // Boot cyclone -- this can lead to rep cbs on recovery
  cyclone_handle = cyclone_boot(config_server_path,
				config_client_path,
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_commit_cb,
				&checkpoint_callback,
				me,
				replicas,
				clients,
				NULL);
  // Listen on port
  void *zmq_context = zmq_init(zmq_threads);
  dispatcher_loop_obj    = new dispatcher_loop();
  dispatcher_loop_obj->zmq_context = zmq_context;
  dispatcher_loop_obj->clients  = clients;
  dispatcher_loop_obj->machines = pt_client.get<int>("machines.machines");
  replica_me = me;
  router = new server_switch(
#if defined(DPDK_STACK)
			     global_dpdk_context,
#else
			     zmq_context,
#endif
			     zmq_context,
			     &pt_server,
			     &pt_client,
			     me,
			     clients,
			     3);
  dispatcher_loop_obj->tx_thread = 
    new boost::thread(boost::ref(*router));
  follower_req_socket = cyclone_socket_out_loopback(zmq_context);
  cyclone_connect_endpoint_loopback(follower_req_socket, "inproc://FOLLOWER");
  follower_rep_socket = cyclone_socket_in_loopback(zmq_context);
  cyclone_bind_endpoint_loopback(follower_rep_socket, "inproc://FOLLOWER");
  (*dispatcher_loop_obj)();
}
