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
#include "checkpoint.hpp"
#include "cyclone_context.hpp"

dpdk_context_t * global_dpdk_context = NULL;
extern struct rte_ring ** to_cores;
extern struct rte_ring *from_cores;
cyclone_t **quorums;
core_status_t *core_status;
static rpc_callbacks_t app_callbacks;
static void client_reply(rpc_t *req, 
			 rpc_t *rep,
			 void *payload,
			 int sz,
			 int port,
			 int q)
{
  rte_mbuf *m = rte_pktmbuf_alloc(global_dpdk_context->mempools[q]);
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for client response";
    exit(-1);
  }
  rep->channel_seq = req->channel_seq;
  if(sz > 0) {
    memcpy(rep + 1, payload, sz);
  }
  cyclone_prep_mbuf_server2client(global_dpdk_context,
				  port,
				  req->requestor,
				  req->client_port,
				  m,
				  rep,
				  sizeof(rpc_t) + sz);
  
  int e = cyclone_tx(global_dpdk_context, port, m, q);
  if(e) {
    BOOST_LOG_TRIVIAL(warning) << "Failed to send response to client";
  }
}

int init_rpc_cookie_info(rpc_cookie_t *cookie, rpc_t *rpc)
{
  cookie->replication = &(rpc->wal.rep);
  cookie->log_idx     = rpc->wal.idx;
  cookie->ret_size    = 0;
  // Multi-core operation ?
  if(rpc->core_mask & (rpc->core_mask - 1)) {
    // Need to wait for sync
    unsigned long mask     = rpc->core_mask;
    unsigned int *snapshot = (unsigned int *)(rpc + 1);
    int core_leader = __builtin_ffsl(mask) - 1;
    core_status_t *cstatus_leader = &core_status[core_leader];
    if(cookie->core_id == core_leader) {
      if(!wait_barrier_leader(cstatus_leader, 
			      rpc2rdv(rpc),
			      cookie->core_id,
			      snapshot,
			      mask))
	return 0;
    }
    else {
      if(!wait_barrier_follower(cstatus_leader, 
				rpc2rdv(rpc),
				cookie->core_id,
				snapshot[core_leader],
				mask))
	  return 0;
    }
  }
  return 1;
}

int exec_rpc_internal(rpc_t *rpc, int len, rpc_cookie_t *cookie, core_status_t *cstatus)
{
  if(!init_rpc_cookie_info(cookie, rpc)) {
    return -1;
  }
  const unsigned char * user_data = (const unsigned char *)(rpc + 1);
  if(rpc->core_mask & (rpc->core_mask - 1)) {
    user_data += sizeof(ic_rdv_t);
  }
  int checkpoint_idx = app_callbacks.rpc_callback(user_data,
						  len,
						  cookie);
  if(rpc->wal.rep == REP_SUCCESS) {    
    cstatus->checkpoint_idx = checkpoint_idx;
    __sync_synchronize(); // publish core status
    return 0;
  }
  else {
    __sync_synchronize(); // publish core status
    app_callbacks.gc_callback(cookie);
    return -1;
  } 
}

int exec_rpc_internal_ro(rpc_t *rpc, int len, rpc_cookie_t *cookie)
{
  if(!init_rpc_cookie_info(cookie, rpc)) {
    return -1;
  }
  const unsigned char * user_data = (const unsigned char *)(rpc + 1);
  app_callbacks.rpc_callback(user_data,
			     len,
			     cookie);
  return 0;
}

typedef struct executor_st {
  rte_mbuf *m;
  rpc_t* client_buffer, *resp_buffer;
  int sz;
  unsigned long quorum;
  unsigned long tid;
  int port_id;
  rpc_cookie_t cookie;
  core_status_t *cstatus;
  int replicas;
  unsigned long QUORUM_TO;

  int compute_quorum_size(int idx)
  {
    int votes = 1; // include me
    for(int i=0;i<replicas;i++) {
      if(i == quorums[quorum]->me)
	continue;
      if(quorums[quorum]->match_indices[i] >= idx) {
	votes++;
      }
    }
    return votes;
  }

  void await_quorum(rpc_t *rpc, int idx)
  {
    return; // TBD
    do {
    } while(compute_quorum_size(idx) < replicas &&
	    (rte_get_tsc_cycles() - rpc->timestamp <= QUORUM_TO));
  }

  void exec()
  {
    cookie.core_id   = tid;
    if(client_buffer->code == RPC_REQ_KICKER) {
      cstatus->exec_term = client_buffer->wal.term;
      while(client_buffer->wal.rep == REP_UNKNOWN);
      return;
    }
    else if(client_buffer->code == RPC_REQ_STABLE) {
      resp_buffer->code = RPC_REP_OK;
      cookie.ret_value  = client_buffer + 1;
      cookie.ret_size   = num_quorums*sizeof(unsigned int);
      client_reply(client_buffer, 
		   resp_buffer, 
		   cookie.ret_value, 
		   cookie.ret_size,
		   port_id,
		   num_queues*num_quorums + tid);
    }
    else if(client_buffer->flags & RPC_FLAG_RO) {
      int e = exec_rpc_internal_ro(client_buffer, sz, &cookie);
      if(client_buffer->wal.leader && !e && (quorums[quorum]->snapshot&1)) {
	resp_buffer->code = RPC_REP_OK;
	client_reply(client_buffer, 
		     resp_buffer, 
		     cookie.ret_value, 
		     cookie.ret_size,
		     port_id,
		     num_queues*num_quorums + tid);
      }
      if(!e) {
	app_callbacks.gc_callback(&cookie);
      }
    }
    else if(client_buffer->code == RPC_REQ_NODEDEL || 
	    client_buffer->code == RPC_REQ_NODEADD) {
      cstatus->exec_term = client_buffer->wal.term;
      while(client_buffer->wal.rep == REP_UNKNOWN);
      if(client_buffer->wal.leader &&
	 client_buffer->wal.rep == REP_SUCCESS &&
	 (quorums[quorum]->snapshot&1)) {
	resp_buffer->code = RPC_REP_OK;
	client_reply(client_buffer,
		     resp_buffer,
		     NULL,
		     0,
		     port_id,
		     num_queues*num_quorums + tid);
      }
    }
    else {
      cstatus->exec_term = client_buffer->wal.term;
      int e = exec_rpc_internal(client_buffer, sz, &cookie, cstatus);
      if(client_buffer->wal.leader && !e && (quorums[quorum]->snapshot&1)) {
	await_quorum(client_buffer, client_buffer->wal.idx);
	resp_buffer->code = RPC_REP_OK;
	client_reply(client_buffer, 
		     resp_buffer, 
		     cookie.ret_value, 
		     cookie.ret_size,
		     port_id,
		     num_queues*num_quorums + tid);
      }
      if(!e) {
	app_callbacks.gc_callback(&cookie);
      }
    }
  }

  void operator() ()
  {
    resp_buffer = (rpc_t *)malloc(MSG_MAXSIZE);
    while(true) {
      int e = rte_ring_sc_dequeue(to_cores[tid], (void **)&quorum);
      if(e == 0) {
	while(rte_ring_sc_dequeue(to_cores[tid], (void **)&m) != 0);
	while(rte_ring_sc_dequeue(to_cores[tid], (void **)&client_buffer) != 0);
	sz = client_buffer->payload_sz;
	cstatus = &core_status[tid];
	client_buffer->timestamp = rte_get_tsc_cycles();
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


void cyclone_network_init(const char *config_cluster_path,
			  int ports,
			  int me_mc,
			  int queues)
{
  boost::property_tree::ptree pt_cluster;
  boost::property_tree::read_ini(config_cluster_path, pt_cluster);
  char key[150];
  global_dpdk_context = (dpdk_context_t *)malloc(sizeof(dpdk_context_t));
  global_dpdk_context->me = me_mc;
  int cluster_machines = pt_cluster.get<int>("machines.count");
  global_dpdk_context->ports = ports;
  global_dpdk_context->mc_addresses = (struct ether_addr **)
    malloc(cluster_machines*sizeof(struct ether_addr *));
  int config_ports = pt_cluster.get<int>("machines.ports");
  for(int i=0;i<cluster_machines;i++) {
    global_dpdk_context->mc_addresses[i] = (struct ether_addr *)
      malloc(config_ports*sizeof(struct ether_addr));
    for(int j=0;j<config_ports;j++) {
      sprintf(key, "machines.addr%d_%d", i, j);
      std::string s = pt_cluster.get<std::string>(key);
      unsigned int bytes[6];
      sscanf(s.c_str(),
	     "%02X:%02X:%02X:%02X:%02X:%02X",
	     &bytes[0], &bytes[1], &bytes[2], &bytes[3], &bytes[4], &bytes[5]);
      global_dpdk_context->mc_addresses[i][j].addr_bytes[0] = bytes[0];
      global_dpdk_context->mc_addresses[i][j].addr_bytes[1] = bytes[1];
      global_dpdk_context->mc_addresses[i][j].addr_bytes[2] = bytes[2];
      global_dpdk_context->mc_addresses[i][j].addr_bytes[3] = bytes[3];
      global_dpdk_context->mc_addresses[i][j].addr_bytes[4] = bytes[4];
      global_dpdk_context->mc_addresses[i][j].addr_bytes[5] = bytes[5];
      BOOST_LOG_TRIVIAL(info) << "CYCLONE::COMM::DPDK Cluster machine "
			      << s.c_str();
    }
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
  static PMEMobjpool *state;
  std::string file_path = pt_quorum.get<std::string>("dispatch.filepath");
  unsigned long heapsize = pt_quorum.get<unsigned long>("dispatch.heapsize");
  char me_str[100];
  sprintf(me_str,"%d", me);
  file_path.append(me_str);
  app_callbacks = *rpc_callbacks;
  bool i_am_active = false;
  char ringname[50];

  BOOST_LOG_TRIVIAL(info) << "Dispatcher start. sizeof(rpc_t) is :" << sizeof(rpc_t);

  // Initialize comm rings
  
  to_cores = (struct rte_ring **)malloc(executor_threads*sizeof(struct rte_ring *));

  sprintf(ringname, "FROM_CORES");
  from_cores =  rte_ring_create(ringname, 
				65536,
				rte_socket_id(), 
				RING_F_SC_DEQ);
  for(int i=0;i<executor_threads;i++) {
    sprintf(ringname, "TO_CORE%d", i);
    to_cores[i] =  rte_ring_create(ringname, 
				   65536,
				   rte_socket_id(), 
				   RING_F_SC_DEQ); 
  }

  to_quorums = (struct rte_ring **)malloc(num_quorums*sizeof(struct rte_ring *));
  for(int i=0;i<num_quorums;i++) {
    sprintf(ringname, "TO_QUORUM%d", i);
    to_quorums[i] =  rte_ring_create(ringname, 
				     65536,
				     rte_socket_id(), 
				     RING_F_SP_ENQ|RING_F_SC_DEQ); 
  }

  
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
    BOOST_LOG_TRIVIAL(info) << "DISPATCHER: Recovered state";
  }
  
  quorums = (cyclone_t **)malloc(num_quorums*sizeof(cyclone_t *));
  core_status = (core_status_t *)malloc(executor_threads*sizeof(core_status_t));
  for(int i=0;i < executor_threads;i++) {
    core_status[i].exec_term      = 0;
    core_status[i].checkpoint_idx = -1;
    memset(&core_status[i].nonce, 0, sizeof(ic_rdv_t));
    core_status[i].stable  = 0;
    core_status[i].success = 0;
    core_status[i].barrier[0] = 0;
    core_status[i].barrier[1] = 0;
  }
  
  
  for(int i=0;i<num_quorums;i++) {
    quorum_switch *router = new quorum_switch(&pt_cluster, &pt_quorum);
    cyclone_setup(config_quorum_path,
		  router,
		  i,
		  me,
		  clients,
		  NULL);
  }
  cyclone_boot();
  
  double tsc_mhz = (rte_get_tsc_hz()/1000000.0);
  unsigned long QUORUM_TO = RAFT_QUORUM_TO*tsc_mhz;
  
  for(int i=0;i < executor_threads;i++) {
    executor_t *ex = new executor_t();
    ex->tid = i;
    ex->port_id = i % global_dpdk_context->ports; 
    ex->replicas =  pt_quorum.get<int>("active.replicas");
    ex->QUORUM_TO = QUORUM_TO;
    int e = rte_eal_remote_launch(dpdk_executor, (void *)ex, 1 + num_quorums + i);
    if(e != 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to launch executor on remote lcore";
      exit(-1);
    }
  }
  rte_eal_mp_wait_lcore();
}

