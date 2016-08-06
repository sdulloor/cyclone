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
#include "tuning.hpp"
#include "checkpoint.hpp"
#include "cyclone_context.hpp"

static void *cyclone_handle;
static boost::property_tree::ptree pt_server;
static boost::property_tree::ptree pt_client;

void * global_dpdk_context;
extern struct rte_ring ** to_cores;
extern struct rte_ring *from_cores;
static server_switch *router;
static PMEMobjpool *state;
static rpc_callbacks_t app_callbacks;
static volatile bool building_image = false;
static void client_reply(rpc_t *req, 
			 rpc_t *rep,
			 void *payload,
			 int sz,
			 int q)
{
  rep->client_id   = req->client_id;
  rep->client_txid = req->client_txid;
  rep->channel_seq = req->channel_seq;
  if(sz > 0) {
    memcpy(rep + 1, payload, sz);
  }
  router->direct_send_data(req->requestor,
			   req->client_port,
			   (void *)rep, 
			   sizeof(rpc_t) + sz,
			   q); 
}

void init_rpc_cookie_info(rpc_cookie_t *cookie, rpc_t *rpc)
{
  cookie->raft_idx  = rpc->wal.raft_idx;
  cookie->raft_term = rpc->wal.raft_term;
  cookie->client_id = rpc->client_id;
  cookie->client_txid = rpc->client_txid;
}

int exec_rpc_internal(rpc_t *rpc, int len, rpc_cookie_t *cookie)
{
  while(building_image);
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  void *tx_handle;
  init_rpc_cookie_info(cookie, rpc);
  tx_handle = app_callbacks.rpc_callback((const unsigned char *)(rpc + 1),
					 len - sizeof(rpc_t),
					 cookie);
  while(rpc->wal.rep == REP_UNKNOWN);
   
  if(rpc->wal.rep == REP_SUCCESS) {
    app_callbacks.tx_commit(tx_handle, cookie);
    return 0;
  }
  else {
    app_callbacks.tx_abort(tx_handle);
    return -1;
  } 
}

void exec_rpc_internal_ro(rpc_t *rpc, int len, rpc_cookie_t *cookie)
{
  while(building_image);
  init_rpc_cookie_info(cookie, rpc);
  app_callbacks.rpc_callback((const unsigned char *)(rpc + 1),
			     len - sizeof(rpc_t),
			     cookie);
}

struct dispatcher_loop {
  int clients;
  int machines;

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

typedef struct executor_st {
  rte_mbuf *m;
  rpc_t* client_buffer, *resp_buffer;
  int sz;
  unsigned long tid;
  rpc_cookie_t cookie;

  void exec()
  {
    if(client_buffer->code == RPC_REQ_LAST_TXID) {
      resp_buffer->code = RPC_REP_COMPLETE;
      cookie.client_id = client_buffer->client_id;
      app_callbacks.cookie_get_callback(&cookie);
      resp_buffer->last_client_txid = cookie.client_txid;
      client_reply(client_buffer, 
		   resp_buffer, 
		   NULL, 
		   0,
		   num_queues + tid);
    }
    else if(client_buffer->code == RPC_REQ_STATUS) {
      cookie.client_id = client_buffer->client_id;
      app_callbacks.cookie_get_callback(&cookie);
      resp_buffer->last_client_txid = client_buffer->client_txid;
      if(cookie.client_txid < client_buffer->client_txid) {
	resp_buffer->code = RPC_REP_UNKNOWN;
	client_reply(client_buffer, 
		     resp_buffer, 
		     NULL, 
		     0,
		     num_queues + tid);
      }
      else if(cookie.client_txid == client_buffer->client_txid) {
	resp_buffer->code = RPC_REP_COMPLETE;
	client_reply(client_buffer, 
		     resp_buffer, 
		     cookie.ret_value, 
		     cookie.ret_size,
		     num_queues + tid);
      }
      else {
	resp_buffer->code = RPC_REP_OLD;
	client_reply(client_buffer, 
		     resp_buffer, 
		     NULL,
		     0,
		     num_queues + tid);
      }
    }
    else if(client_buffer->code == RPC_REQ_NOOP) {
      if((client_buffer->flags & RPC_FLAG_RO) == 0) {
	while(client_buffer->wal.rep == REP_UNKNOWN);
	if(client_buffer->wal.leader) {
	  if(client_buffer->wal.rep == REP_SUCCESS) {
	    resp_buffer->code = RPC_REP_COMPLETE;
	    client_reply(client_buffer, resp_buffer, NULL, 0, num_queues + tid);
	  }
	  else {
	    resp_buffer->code = RPC_REP_UNKNOWN;
	    client_reply(client_buffer, resp_buffer, NULL, 0, num_queues + tid);
	  }
	}
      }
      else {
	if(client_buffer->wal.leader) {
	  resp_buffer->code = RPC_REP_COMPLETE;
	  client_reply(client_buffer, resp_buffer, NULL, 0, num_queues + tid);
	}
      }
    }
    else {
      // Exactly once RPC check
      cookie.client_id = client_buffer->client_id;
      app_callbacks.cookie_get_callback(&cookie);
      if(cookie.client_txid > client_buffer->client_txid) {
	if(client_buffer->wal.leader) {
	  resp_buffer->code = RPC_REP_OLD;
	  client_reply(client_buffer, 
		       resp_buffer, 
		       cookie.ret_value, 
		       cookie.ret_size,
		       num_queues + tid);
	}
      }
      else if(cookie.client_txid == client_buffer->client_txid) {
	if(client_buffer->wal.leader) {
	  resp_buffer->code = RPC_REP_COMPLETE;
	  client_reply(client_buffer, 
		       resp_buffer, 
		       cookie.ret_value, 
		       cookie.ret_size,
		       num_queues + tid);
	}
      }
      else if(client_buffer->flags & RPC_FLAG_RO) {
	exec_rpc_internal_ro(client_buffer, sz, &cookie);
	if(client_buffer->wal.leader) {
	  resp_buffer->code = RPC_REP_COMPLETE;
	  client_reply(client_buffer, 
		       resp_buffer, 
		       cookie.ret_value, 
		       cookie.ret_size,
		       num_queues + tid);
	}
	if(cookie.ret_size > 0) {
	  free(cookie.ret_value);
	}
      }
      else {
	int e = exec_rpc_internal(client_buffer, sz, &cookie);
	if(client_buffer->wal.leader) {
	  if(e) {
	    resp_buffer->code = RPC_REP_UNKNOWN;
	  }
	  else {
	    resp_buffer->code = RPC_REP_COMPLETE;
	  }
	  client_reply(client_buffer, 
		       resp_buffer, 
		       cookie.ret_value, 
		       cookie.ret_size,
		       num_queues + tid);
	}
	if(cookie.ret_size > 0) {
	  free(cookie.ret_value);
	}
      }
    }
  }

  void operator() ()
  {
    resp_buffer = (rpc_t *)malloc(MSG_MAXSIZE);
    while(true) {
      int e = rte_ring_sc_dequeue(to_cores[tid], (void **)&m);
      if(e == 0) {
	while(rte_ring_sc_dequeue(to_cores[tid], (void **)&client_buffer) != 0);
	sz = client_buffer->payload_sz;
	exec();
	rte_pktmbuf_free_seg(m);
      }
    }
  }
} executor_t;

int dpdk_executor(void *arg)
{
  executor_t *ex = (executor_t *)arg;
  (*ex)();
  return 0;
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
  global_dpdk_context = dpdk_context(sizeof(struct ether_hdr) +
				     sizeof(struct ipv4_hdr) +
				     sizeof(msg_t) + 
				     sizeof(msg_entry_t) + 
				     MSG_MAXSIZE,
				     (MSG_MAXSIZE + sizeof(rpc_t) - 1)/sizeof(rpc_t));
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  init_checkpoint(file_path.c_str(), me);
  app_callbacks = *rpc_callbacks;
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
  
  cyclone_handle = cyclone_boot(config_server_path,
				config_client_path,
				&checkpoint_callback,
				me,
				replicas,
				clients,
				NULL);
  // Listen on port
  router = new server_switch(global_dpdk_context,
			     &pt_server,
			     &pt_client,
			     me,
			     clients);
  for(int i=0;i < executor_threads;i++) {
    executor_t *ex = new executor_t();
    ex->tid = i;
    int e = rte_eal_remote_launch(dpdk_executor, (void *)ex, 3 + i);
    if(e != 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to launch executor on remote lcore";
      exit(-1);
    }
  }
  rte_eal_mp_wait_lcore();
}
