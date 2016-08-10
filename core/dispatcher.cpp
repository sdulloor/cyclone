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
dpdk_context_t * global_dpdk_context = NULL;
extern struct rte_ring ** to_cores;
extern struct rte_ring *from_cores;
static quorum_switch *router;
static PMEMobjpool *state;
static rpc_callbacks_t app_callbacks;
static volatile int sending_checkpoints = 0;
static void client_reply(rpc_t *req, 
			 rpc_t *rep,
			 void *payload,
			 int sz,
			 int q)
{
  rte_mbuf *m = rte_pktmbuf_alloc(global_dpdk_context->mempools[q]);
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for client response";
    exit(-1);
  }
  rep->client_id   = req->client_id;
  rep->client_txid = req->client_txid;
  rep->channel_seq = req->channel_seq;
  if(sz > 0) {
    memcpy(rep + 1, payload, sz);
  }
  cyclone_prep_mbuf(global_dpdk_context,
		    req->requestor,
		    req->client_port,
		    m,
		    rep,
		    sizeof(rpc_t) + sz);
		    
  cyclone_tx(global_dpdk_context, m, q);
}

void init_rpc_cookie_info(rpc_cookie_t *cookie, rpc_t *rpc)
{
  cookie->client_id = rpc->client_id;
  cookie->client_txid = rpc->client_txid;
}

int exec_rpc_internal(rpc_t *rpc, int len, rpc_cookie_t *cookie)
{
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
  init_rpc_cookie_info(cookie, rpc);
  app_callbacks.rpc_callback((const unsigned char *)(rpc + 1),
			     len - sizeof(rpc_t),
			     cookie);
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
	if(client_buffer->code == RPC_REQ_NODEDEL || client_buffer->code == RPC_REQ_NODEADD) {
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


void cyclone_network_init(const char *config_cluster_path, int me_mc, int queues)
{
  boost::property_tree::ptree pt_cluster;
  boost::property_tree::read_ini(config_cluster_path, pt_cluster);
  char key[150];
  global_dpdk_context = (dpdk_context_t *)malloc(sizeof(dpdk_context_t));
  global_dpdk_context->me = me_mc;
  int cluster_machines = pt_cluster.get<int>("machines.count");
  global_dpdk_context->mc_addresses = (struct ether_addr *)
    malloc(cluster_machines*sizeof(struct ether_addr));
  for(int i=0;i<cluster_machines;i++) {
    sprintf(key, "machines.addr%d", i);
    std::string s = pt_cluster.get<std::string>(key);
    unsigned int bytes[6];
    sscanf(s.c_str(),
	   "%02X:%02X:%02X:%02X:%02X:%02X",
	   &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
    global_dpdk_context->mc_addresses[i].addr_bytes[0] = bytes[0];
    global_dpdk_context->mc_addresses[i].addr_bytes[1] = bytes[1];
    global_dpdk_context->mc_addresses[i].addr_bytes[2] = bytes[2];
    global_dpdk_context->mc_addresses[i].addr_bytes[3] = bytes[3];
    global_dpdk_context->mc_addresses[i].addr_bytes[4] = bytes[4];
    global_dpdk_context->mc_addresses[i].addr_bytes[5] = bytes[5];
    BOOST_LOG_TRIVIAL(info) << "CYCLONE::COMM::DPDK Cluster machine "
                            << s.c_str();
  }
  dpdk_context_init(global_dpdk_context,
		    sizeof(struct ether_hdr) +
		    sizeof(struct ipv4_hdr) +
		    sizeof(msg_t) + 
		    sizeof(msg_entry_t) + 
		    MSG_MAXSIZE,
		    (MSG_MAXSIZE + sizeof(rpc_t) - 1)/sizeof(rpc_t),
		    queues);
}

void dispatcher_start(const char* config_cluster_path,
		      const char* config_quorum_path,
		      rpc_callbacks_t *rpc_callbacks,
		      int me,
		      int me_mc,
		      int clients)
{
  boost::property_tree::ptree pt_cluster;
  boost::property_tree::ptree pt_quorum;
  std::stringstream key;
  std::stringstream addr;
  boost::property_tree::read_ini(config_cluster_path, pt_cluster);
  boost::property_tree::read_ini(config_quorum_path, pt_quorum);
  // Load/Setup state
  std::string file_path = pt_quorum.get<std::string>("dispatch.filepath");
  unsigned long heapsize = pt_quorum.get<unsigned long>("dispatch.heapsize");
  char me_str[100];
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  app_callbacks = *rpc_callbacks;
  bool i_am_active = false;
  for(int i=0;i<pt_quorum.get<int>("active.replicas");i++) {
    char nodeidxkey[100];
    sprintf(nodeidxkey, "active.entry%d",i);
    int nodeidx = pt_quorum.get<int>(nodeidxkey);
    if(nodeidx == me) {
      i_am_active = true;
    }
  }

  if(!i_am_active) {
    BOOST_LOG_TRIVIAL(info) << "Starting inactive server";
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

  router = new quorum_switch(&pt_cluster, &pt_quorum);
  cyclone_handle = cyclone_boot(config_quorum_path,
				router,
				me,
				clients,
				NULL);
  for(int i=0;i < executor_threads;i++) {
    executor_t *ex = new executor_t();
    ex->tid = i;
    int e = rte_eal_remote_launch(dpdk_executor, (void *)ex, 2 + i);
    if(e != 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to launch executor on remote lcore";
      exit(-1);
    }
  }
  rte_eal_mp_wait_lcore();
}
