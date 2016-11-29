#ifndef _CYCLONE_CONTEXT_HPP_
#define _CYCLONE_CONTEXT_HPP_

#include <string>
#include <libpmemobj.h>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
extern "C" {
#include <raft.h>
}
#include <unistd.h>
#include "pmem_layout.h"
#include "circular_log.h"
#include "clock.hpp"
#include "cyclone_comm.hpp"
#include "tuning.hpp"
#include "cyclone.hpp"
#include <rte_cycles.h>

/* Message format */

typedef struct msg_st
{
  int msg_type;
  int source;
  union
  {
    msg_requestvote_t rv;
    msg_requestvote_response_t rvr;
    msg_appendentries_t ae;
    msg_appendentries_response_t aer;
  };
} msg_t;

static rpc_t * pkt2rpc(rte_mbuf *m)
{
  int payload_offset = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr);
  return rte_pktmbuf_mtod_offset(m, rpc_t *, payload_offset);
}

static int pkt2rpcsz(rte_mbuf *m)
{
  int payload_offset = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr);
  return m->data_len - payload_offset; 
}

static rpc_t * pktadj2rpc(rte_mbuf *m)
{
  int payload_offset = sizeof(struct ipv4_hdr) + sizeof(msg_t) + sizeof(msg_entry_t);
  return rte_pktmbuf_mtod_offset(m, rpc_t *, payload_offset);
}

static msg_t *pktadj2msg(rte_mbuf *m)
{
  int payload_offset = sizeof(struct ipv4_hdr);
  return rte_pktmbuf_mtod_offset(m, msg_t *, payload_offset);
}

static int pktadj2rpcsz(rte_mbuf *m)
{
  int payload_offset = sizeof(struct ipv4_hdr) + sizeof(msg_t) + sizeof(msg_entry_t);
  return m->data_len - payload_offset; 
}

static void pktsetrpcsz(rte_mbuf *m, int sz)
{
  int payload_offset = sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr);
  m->data_len = payload_offset + sz;
  m->pkt_len  = m->data_len;
}

static void adjust_head(rte_mbuf *m)
{
  if(rte_pktmbuf_adj(m, sizeof(struct ether_hdr)) == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to adj ethe hdr";
    exit(-1);
  }
  msg_t *hdr = (msg_t *)rte_pktmbuf_prepend(m, sizeof(msg_t) + sizeof(msg_entry_t));
  if(hdr == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to prepend msg_t";
    exit(-1);
  }
}

static void del_adj_header(rte_mbuf *m)
{
  if(rte_pktmbuf_adj(m, sizeof(struct ipv4_hdr) + sizeof(msg_t) + sizeof(msg_entry_t)) == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to adj ethe hdr";
    exit(-1);
  }
}

static void drop_eth_header(rte_mbuf *m)
{
  if(rte_pktmbuf_adj(m, sizeof(struct ether_hdr)) == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to adj ethe hdr";
    exit(-1);
  }
}

/* Message types */
const int  MSG_REQUESTVOTE              = 1;
const int  MSG_REQUESTVOTE_RESPONSE     = 2;
const int  MSG_APPENDENTRIES            = 3;
const int  MSG_APPENDENTRIES_RESPONSE   = 4;

extern struct rte_ring ** to_cores;
extern struct rte_ring *from_cores;
extern dpdk_context_t *global_dpdk_context;

struct cyclone_monitor;
struct cyclone_st;
extern cyclone_st ** quorums;

typedef struct cyclone_st {
  boost::property_tree::ptree pt;
  boost::property_tree::ptree pt_client;
  quorum_switch *router;
  int replicas;
  int me;
  int me_quorum;
  int me_port;
  boost::thread *checkpoint_thread;
  int RAFT_LOGENTRIES;
  raft_pstate_t *pop_raft_state;
  struct circular_log *log;
  raft_server_t *raft_handle;
  void *user_arg;
  unsigned char* cyclone_buffer_out;
  unsigned char* cyclone_buffer_in;
  cyclone_monitor *monitor_obj;
  volatile int sending_checkpoints;

  msg_t ae_responses[PKT_BURST];
  int ae_response_sources[PKT_BURST];
  int ae_response_cnt;

  unsigned long completions;
  unsigned long mark;
  
  volatile unsigned int snapshot;
  volatile int is_quorum_leader;

  int my_q(int q)
  {
    return num_queues*me_quorum + q;
  }
  
  void send_msg(msg_t *msg, int dst_replica)
  {
    rte_mbuf *m = rte_pktmbuf_alloc(global_dpdk_context->mempools[my_q(q_raft)]);
    if(m == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for send mesg";
    }
    cyclone_prep_mbuf(global_dpdk_context, 
		      me_port,
		      router->replica_mc(dst_replica), 
		      my_q(q_raft), 
		      m, 
		      msg, 
		      sizeof(msg_t));
    cyclone_tx(global_dpdk_context, me_port, m, my_q(q_raft));
  }
  
  void send_ae_responses()
  {
    if(ae_response_cnt == 0)
      return;
    // Compact to find the latest response
    int idx     = ae_responses[0].aer.current_idx;
    int to_send = 0;
    int merge_term = ae_responses[0].aer.term;
    for(int i=1;i<ae_response_cnt;i++) {
      if(ae_responses[i].aer.term != merge_term) {
	to_send = -1;
	break;
      }
      if(ae_responses[i].aer.current_idx > idx)
	to_send = i;
    }
    if(to_send != -1) {
      send_msg(&ae_responses[to_send], ae_response_sources[to_send]);
    }
    else {
      for(int i=0;i<ae_response_cnt;i++) {
	send_msg(&ae_responses[i], ae_response_sources[i]);
      }
    }
    ae_response_cnt = 0;
  }

  cyclone_st()
  {}

  /* Handle incoming message and send appropriate response */
  void handle_incoming(rte_mbuf *m)
  {
    drop_eth_header(m);
    msg_t *msg = pktadj2msg(m);
    msg_t resp;
    unsigned long rep;
    unsigned char *payload     = (unsigned char *)(msg + 1);
    unsigned long payload_size = m->pkt_len - 
      (sizeof(struct ipv4_hdr) + sizeof(msg_t)); 
    int e; // TBD: need to handle errors
    char *ptr;
    msg_entry_t client_req;
    msg_entry_response_t *client_rep;
    msg_entry_t *messages; 
    rpc_t *rpc;
    int source = msg->source;;
    int free_buf = 0;

    if(msg->msg_type != MSG_APPENDENTRIES) {
      send_ae_responses();
    }

    switch (msg->msg_type) {
    case MSG_REQUESTVOTE:
      resp.msg_type = MSG_REQUESTVOTE_RESPONSE;
      e = raft_recv_requestvote(raft_handle, 
				raft_get_node(raft_handle, msg->source), 
				&msg->rv, 
				&resp.rvr);
      /* send response */
      resp.source = me;
      rte_pktmbuf_free(m);
      send_msg(&resp, source);
      break;
    case MSG_REQUESTVOTE_RESPONSE:
      e = raft_recv_requestvote_response(raft_handle, 
					 raft_get_node(raft_handle, msg->source), 
					 &msg->rvr);
      rte_pktmbuf_free(m);
      break;
    case MSG_APPENDENTRIES:
    ae_responses[ae_response_cnt].msg_type = MSG_APPENDENTRIES_RESPONSE;
    if(msg->ae.n_entries > 0) {
      msg->ae.entries = (msg_entry_t *)payload;
      msg->ae.entries[0].data.buf = (void *)m;
    }
    else {
      free_buf = 1;
    }
    e = raft_recv_appendentries(raft_handle, 
				raft_get_node(raft_handle, msg->source), 
				&msg->ae, 
				&ae_responses[ae_response_cnt].aer);
    if(free_buf) {
      rte_pktmbuf_free(m);
    }
    ae_responses[ae_response_cnt].source = me;
    ae_response_sources[ae_response_cnt++] = source;
    break;
    case MSG_APPENDENTRIES_RESPONSE:
      e = raft_recv_appendentries_response(raft_handle, 
					   raft_get_node(raft_handle, msg->source), 
					   &msg->aer);
      rte_pktmbuf_free(m);
      break;
    default:
      printf("unknown msg in handle remote code=%d\n", msg->msg_type);
      exit(0);
    }
  }
}cyclone_t;

// Non blocking, best effort
static int take_snapshot(unsigned int *snapshot)
{
  for(int i=0;i<num_quorums;i++) {
    snapshot[i] = quorums[i]->snapshot;
    if((snapshot[i] & 1) == 0) {
      return 0;
    }
  }
  __sync_synchronize();
  // check snapshot
  for(int i=0;i<num_quorums;i++) {
    if(snapshot[i] != quorums[i]->snapshot) {
      return 0;
    }
  }
  return 1; // success
}
  
struct cyclone_monitor {
  volatile bool terminate;
  cyclone_t *cyclone_handle;
  void *poll_items;

  cyclone_monitor()
  :terminate(false)
  {}

  void compact(rte_mbuf *m)
  {
    rte_mbuf *next = m->next, *temp;
    while(next) {
      rte_memcpy(rte_pktmbuf_mtod_offset(m, void *, m->data_len), 
		 rte_pktmbuf_mtod(next, void *),
		 next->data_len);
      m->data_len += next->data_len;
      temp = next;
      next = next->next;
      temp->next = NULL;
      rte_pktmbuf_free(temp);
    }
    m->pkt_len = m->data_len;
    m->nb_segs = 1;
    m->next = NULL;
  }

  int bad(rte_mbuf *m)
  {
    rte_prefetch0(rte_pktmbuf_mtod(m, void *));
    struct ether_hdr *e = rte_pktmbuf_mtod(m, struct ether_hdr *);
    struct ipv4_hdr *ip = (struct ipv4_hdr *)(e + 1);
    if(e->ether_type != rte_cpu_to_be_16(ETHER_TYPE_IPv4)) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk. Protocol mismatch";
      return -1;
    }
    else if(ip->src_addr != magic_src_ip) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk. non magic ip";
      rte_pktmbuf_free(m);
      return -1;
    }
    else if(m->data_len <= sizeof(struct ether_hdr) + sizeof(struct ipv4_hdr)) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping junk = pkt size too small";
      rte_pktmbuf_free(m);
      return -1;
    }
    else if(m->next != NULL) {
      BOOST_LOG_TRIVIAL(warning) << "Dropping multiseg packet at rx";
      BOOST_LOG_TRIVIAL(warning) << "Pkt len = " 
				 << m->pkt_len
				 <<" seg len = "
				 <<  m->data_len;
      rte_pktmbuf_free(m);
      return -1;
    }
    return 0;
  }

  void publish_snapshot()
  {
    unsigned int current_term = raft_get_current_term(cyclone_handle->raft_handle);
    __sync_synchronize();
    if(cyclone_get_leader(cyclone_handle) == -1) 
      return; // Still in leader election 
    int is_leader    = cyclone_is_leader(cyclone_handle);
    if(current_term == raft_get_current_term(cyclone_handle->raft_handle)) {
      cyclone_handle->snapshot      = (current_term << 1) + (is_leader ? 1:0);
      __sync_synchronize();
    }
  }

  void operator ()()
  {
    unsigned long mark = rte_get_tsc_cycles();
    double tsc_mhz = (rte_get_tsc_hz()/1000000.0);
    unsigned long PERIODICITY_CYCLES = PERIODICITY*tsc_mhz;
    unsigned long elapsed_time;
    msg_entry_t *messages;
    rte_mbuf *pkt_array[PKT_BURST], *m, *chain_tail;
    int chain_size[2*PKT_BURST];
    messages = (msg_entry_t *)malloc(2*PKT_BURST*sizeof(msg_entry_t));
    int available;
    int accepted;
    int is_leader;
    unsigned int current_term;
    while(!terminate) {
      // Handle any outstanding requests
      available = rte_eth_rx_burst(cyclone_handle->me_port,
				   cyclone_handle->my_q(q_raft),
				   &pkt_array[0],
				   PKT_BURST);
      cyclone_handle->ae_response_cnt = 0;
      for(int i=0;i<available;i++) {
	m = pkt_array[i];
	if(bad(m)) {
	  rte_pktmbuf_free(m);
	  continue;
	}
	cyclone_handle->handle_incoming(m);
      }
      cyclone_handle->send_ae_responses();

      accepted  = 0;
      memset(chain_size, 0, 2*PKT_BURST);
     
      // Publish snapshot
      current_term = raft_get_current_term(cyclone_handle->raft_handle);
      if(current_term != (cyclone_handle->snapshot >> 1)) {
	publish_snapshot();
      }
      // Leadership change ?
      is_leader    = cyclone_is_leader(cyclone_handle);
      if(is_leader != cyclone_handle->is_quorum_leader) {
	if(!cyclone_handle->is_quorum_leader) {
	  // Became leader -- send kicker
	  rte_mbuf *k = rte_pktmbuf_alloc(global_dpdk_context->mempools[cyclone_handle->my_q(q_raft)]);
	  if(k == NULL) {
	    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for kicker";
	  }
	  rpc_t *k_rpc = (rpc_t *)rte_pktmbuf_mtod_offset(k, void *, sizeof(ether_hdr) + sizeof(ipv4_hdr));
	  k_rpc->code  = RPC_REQ_KICKER;
	  k_rpc->flags = 0;
	  k_rpc->payload_sz = 0;
	  pktsetrpcsz(k, sizeof(rpc_t));
	  adjust_head(k);
	  messages[0].data.buf = (void *)k;
	  messages[0].data.len = pktadj2rpcsz(k);
	  messages[0].type = RAFT_LOGTYPE_NORMAL;
	  int e = raft_recv_entry_batch(cyclone_handle->raft_handle, 
					messages, 
					NULL,
					1);
	  if(e != 0) {
	    rte_pktmbuf_free(k);
	  }
	}
	cyclone_handle->is_quorum_leader = is_leader;
	continue;
      }
      while(accepted <= PKT_BURST) {
	available = rte_eth_rx_burst(cyclone_handle->me_port,
				     cyclone_handle->my_q(q_dispatcher),
				     &pkt_array[0],
				     PKT_BURST);
	if(available == 0) {
	  break;
	}
	for(int i=0;i<available;i++) {
	  m = pkt_array[i];
	  if(bad(m)) {
	    rte_pktmbuf_free(m);
	    continue;
	  }
	  adjust_head(m);
	  rpc_t *rpc = pktadj2rpc(m);
	  if(rpc->core_mask & (rpc->core_mask - 1)) {
	    BOOST_LOG_TRIVIAL(fatal) << "Don't know how to handle multi-core operation";
	    exit(-1);
	  }
	  int core = __builtin_ffs(rpc->core_mask) - 1;
	  rpc->wal.leader = 1;
	  if(rpc->flags & RPC_FLAG_RO) {
	    if(is_leader) {
	      void *triple[3];
	      triple[0] = (void *)(unsigned long)cyclone_handle->me_quorum;
	      triple[1] = m;
	      triple[2] = rpc;
	      if(rte_ring_mp_enqueue_bulk(to_cores[core], triple, 3) == -ENOBUFS) {
		BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
		exit(-1);
	      }
	    }
	    else {
	      rte_pktmbuf_free(m);
	    }
	    continue;
	  }
	  else if(rpc->code == RPC_REQ_NODEDEL) {
	    messages[accepted].data.buf = (void *)m;
	    messages[accepted].data.len = pktadj2rpcsz(m);
	    messages[accepted].type = RAFT_LOGTYPE_REMOVE_NODE;
	    chain_tail = m;
	    accepted++;
	  }
	  else if(rpc->code == RPC_REQ_NODEADD) {
	    messages[accepted].data.buf = (void *)m;
	    messages[accepted].data.len = pktadj2rpcsz(m);
	    messages[accepted].type = RAFT_LOGTYPE_ADD_NONVOTING_NODE;
	    chain_tail = m;
	    accepted++;
	  }
	  else if(accepted > 0 && 
		  (messages[accepted - 1].data.len + pktadj2rpcsz(m)) <= MSG_MAXSIZE &&
		  messages[accepted - 1].type == RAFT_LOGTYPE_NORMAL &&
		  chain_size[accepted - 1] < PKT_BURST) {
	    rte_mbuf *mhead = (rte_mbuf *)messages[accepted - 1].data.buf;
	    messages[accepted - 1].data.len += pktadj2rpcsz(m);
	    // Wipe out the hdr in the chained packet
	    del_adj_header(m);
	    // Chain to prev packet
	    chain_tail->next = m;
	    chain_tail = m;
	    mhead->nb_segs++;
	    mhead->pkt_len += m->data_len;
	    chain_size[accepted - 1]++;
	    //compact(mprev); // debug
	  }
	  else {
	    messages[accepted].data.buf = (void *)m;
	    messages[accepted].data.len = pktadj2rpcsz(m);
	    messages[accepted].type = RAFT_LOGTYPE_NORMAL;
	    chain_tail = m;
	    accepted++;
	  }
	}
	// Currently the port seems to stall after a BURST
	break;
      }
      if(accepted > 0) {
	int e = raft_recv_entry_batch(cyclone_handle->raft_handle, 
				      messages, 
				      NULL,
				      accepted);
	if(e != 0) {
	  for(int i=0;i<accepted;i++) {
	    rte_pktmbuf_free((rte_mbuf *)messages[i].data.buf);
	  }
	}
      }
      // Handle periodic events -- - AFTER any incoming requests
      elapsed_time = rte_get_tsc_cycles() - mark;
      if(elapsed_time  >= PERIODICITY_CYCLES) {
	raft_periodic(cyclone_handle->raft_handle, (int)(elapsed_time/tsc_mhz));
	mark = rte_get_tsc_cycles();
      }
      // Set preferred leader
      if(cyclone_handle->me_quorum > 0 && quorums[0]->is_quorum_leader) {
	raft_set_preferred_leader(cyclone_handle->raft_handle);
      }
      else {
	raft_unset_preferred_leader(cyclone_handle->raft_handle);
      }
    }
  }
};

#endif
