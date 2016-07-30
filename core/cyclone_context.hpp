#ifndef _CYCLONE_CONTEXT_HPP_
#define _CYCLONE_CONTEXT_HPP_

#include <string>
#include <libpmemobj.h>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <zmq.h>
extern "C" {
#include <raft.h>
}
#include <unistd.h>
#include <boost/thread.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include "pmem_layout.h"
#include "circular_log.h"
#include "clock.hpp"
#include "cyclone_comm.hpp"
#include "tuning.hpp"
#include "runq.hpp"
#include "cyclone.hpp"
#include <rte_cycles.h>

/* Message format */
typedef struct client_io_st {
  void *ptr;
  int size;
  int term;
  int type;
  int* batch_sizes;
} client_t;

typedef struct msg_st
{
  int msg_type;
  union {
    int source;
    int client_port; // Only used for client assist
    unsigned long quorum; // Only used for client assist rep
  };
  union
  {
    msg_requestvote_t rv;
    msg_requestvote_response_t rvr;
    msg_appendentries_t ae;
    msg_appendentries_response_t aer;
    replicant_t rep;
    client_t client;
  };
  msg_entry_response_t * volatile client_rep;
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
const int  MSG_CLIENT_REQ               = 5;
const int  MSG_CLIENT_REQ_BATCH         = 6;
const int  MSG_CLIENT_REQ_TERM          = 7;
const int  MSG_CLIENT_STATUS            = 8;
const int  MSG_CLIENT_REQ_CFG           = 9;
const int  MSG_CLIENT_REQ_SET_IMGBUILD  = 10;
const int  MSG_CLIENT_REQ_UNSET_IMGBUILD= 11;
const int  MSG_ASSISTED_APPENDENTRIES   = 12;
const int  MSG_ASSISTED_QUORUM_OK       = 13;

extern struct rte_ring ** to_cores;
extern struct rte_ring *from_cores;

struct cyclone_monitor;
typedef struct cyclone_st {
  boost::property_tree::ptree pt;
  boost::property_tree::ptree pt_client;
  void *zmq_context;
  raft_switch *router;
  int replicas;
  int me;
  boost::thread *monitor_thread;
  boost::thread *checkpoint_thread;
  unsigned long RAFT_LOGSIZE;
  PMEMobjpool *pop_raft_state;
  raft_server_t *raft_handle;
  cyclone_rep_callback_t cyclone_rep_cb;
  cyclone_callback_t cyclone_pop_cb;
  cyclone_callback_t cyclone_commit_cb;
  void *user_arg;
  unsigned char* cyclone_buffer_out;
  unsigned char* cyclone_buffer_in;
  cyclone_monitor *monitor_obj;
  runq_t<msg_t> comm;

  msg_t ae_responses[PKT_BURST];
  int ae_response_sources[PKT_BURST];
  int ae_response_cnt;
  
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
      cyclone_tx(router->output_socket(ae_response_sources[to_send]),
		 (unsigned char *)&ae_responses[to_send], 
		 sizeof(msg_t), 
		 "APPENDENTRIES RESP");
    }
    else {
      for(int i=0;i<ae_response_cnt;i++) {
	cyclone_tx(router->output_socket(ae_response_sources[i]),
		   (unsigned char *)&ae_responses[i], 
		   sizeof(msg_t), 
		   "APPENDENTRIES RESP");
      }
    }
    ae_response_cnt = 0;
  }

  cyclone_st()
  {}

  unsigned long get_log_offset()
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    return D_RO(log)->log_tail;
  }
  
  unsigned long log_space_left(struct circular_log *log, unsigned long tail)
  {
    if(log->log_head <= tail) {
      return RAFT_LOGSIZE - (tail - log->log_head);
    }
    else {
      return log->log_head - tail;
    }
  }

  void append_to_raft_log(unsigned char *data, int size)
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    unsigned long space_needed = size + 2*sizeof(int);
    unsigned long space_available;
    if(log->log_head <= log->log_tail) {
      space_available = RAFT_LOGSIZE -
	(log->log_tail - log->log_head);
    }
    else {
      space_available =	log->log_head - log->log_tail;
    }
    if(space_available < space_needed) {
      // Overflow !
      BOOST_LOG_TRIVIAL(fatal) << "Out of RAFT logspace !";
      exit(-1);
    }
    unsigned long new_tail = log->log_tail;
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 log->log_tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 data,
			 size);
    new_tail = circular_log_advance_ptr(new_tail, size, RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    persist_to_circular_log(pop_raft_state, log,
			    RAFT_LOGSIZE,
			    log->log_tail,
			    new_tail - log->log_tail);
    log->log_tail = new_tail;
    pmemobj_persist(pop_raft_state,
		    &log->log_tail,
		    sizeof(unsigned long));
  }

  unsigned long append_to_raft_log_noupdate(struct circular_log *log,
					    unsigned char *data,
					    int size,
					    unsigned long tail)
  {
    unsigned long space_needed = size + 2*sizeof(int);
    unsigned long space_available;
    if(log->log_head <= tail) {
      space_available = RAFT_LOGSIZE -
	(tail - log->log_head);
    }
    else {
      space_available =	log->log_head - tail;
    }
    if(space_available < space_needed) {
      // Overflow !
      BOOST_LOG_TRIVIAL(fatal) << "Out of RAFT logspace !";
      exit(-1);
    }
    unsigned long new_tail = tail;
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 data,
			 size);
    new_tail = circular_log_advance_ptr(new_tail, size, RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    return new_tail;
  }

  unsigned long double_append_to_raft_log(unsigned char *data1, 
					  int size1,
					  unsigned char *data2,
					  int size2)
  {
    unsigned long ptr;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    unsigned long space_needed = (size1 + size2) + 4*sizeof(int);
    unsigned long space_available;
    if(log->log_head <= log->log_tail) {
      space_available = RAFT_LOGSIZE -
	(log->log_tail - log->log_head);
    }
    else {
      space_available =	log->log_head - log->log_tail;
    }
    if(space_available < space_needed) {
      // Overflow !
      BOOST_LOG_TRIVIAL(fatal) << "Out of RAFT logspace !";
      exit(-1);
    }
    unsigned long new_tail = log->log_tail;
    ///////
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 log->log_tail,
			 (unsigned char *)&size1,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 data1,
			 size1);
    new_tail = circular_log_advance_ptr(new_tail, size1, RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 (unsigned char *)&size1,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    ////////
    ptr = new_tail;
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 (unsigned char *)&size2,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 data2,
			 size2);
    new_tail = circular_log_advance_ptr(new_tail, size2, RAFT_LOGSIZE);
    copy_to_circular_log(pop_raft_state, log,
			 RAFT_LOGSIZE,
			 new_tail,
			 (unsigned char *)&size2,
			 sizeof(int));
    new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);


    persist_to_circular_log(pop_raft_state, log,
			    RAFT_LOGSIZE,
			    log->log_tail,
			    new_tail - log->log_tail);
    log->log_tail = new_tail;
    pmemobj_persist(pop_raft_state,
		    &log->log_tail,
		    sizeof(unsigned long));
    return ptr;
  }

  void remove_head_raft_log()
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log* log = D_RW(tmp);
    if(log->log_head != log->log_tail) {
      int size;
      copy_from_circular_log(log,
			     RAFT_LOGSIZE,
			     (unsigned char *)&size,
			     log->log_head,
			     sizeof(int)); 
      log->log_head = circular_log_advance_ptr
	(log->log_head, 2*sizeof(int) + size, RAFT_LOGSIZE);
      pmemobj_persist(pop_raft_state,
		      &log->log_head,
		      sizeof(unsigned long));
    }
  }

  void double_remove_head_raft_log()
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    unsigned long newhead = log->log_head;
    if(newhead != log->log_tail) {
      int size;
      copy_from_circular_log(log,
			     RAFT_LOGSIZE,
			     (unsigned char *)&size,
			     newhead,
			     sizeof(int)); 
      newhead = circular_log_advance_ptr
	(newhead, 2*sizeof(int) + size, RAFT_LOGSIZE);
    }
    
    if(newhead != log->log_tail) {
      int size;
      copy_from_circular_log(log,
			     RAFT_LOGSIZE,
			     (unsigned char *)&size,
			     newhead,
			     sizeof(int)); 
      newhead = circular_log_advance_ptr
	(newhead, 2*sizeof(int) + size, RAFT_LOGSIZE);
    }
    log->log_head = newhead;
    clflush(&log->log_head, sizeof(unsigned long));
  }

  void remove_tail_raft_log()
  {
    int result = 0;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    if(log->log_head != log->log_tail) {
      int size;
      unsigned long new_tail = log->log_tail;
      new_tail = circular_log_recede_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
      copy_from_circular_log(log, RAFT_LOGSIZE,
			     (unsigned char *)&size, new_tail, sizeof(int)); 
      new_tail = circular_log_recede_ptr(new_tail, size + sizeof(int), RAFT_LOGSIZE);
      log->log_tail = new_tail;
      pmemobj_persist(pop_raft_state,
		      &log->log_tail,
		      sizeof(unsigned long));
    }
  }

  void double_remove_tail_raft_log()
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    unsigned long new_tail = log->log_tail;

    if(log->log_head != new_tail) {
      int size;
        new_tail = circular_log_recede_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
      copy_from_circular_log(log, RAFT_LOGSIZE,
			     (unsigned char *)&size, new_tail, sizeof(int)); 
      new_tail = circular_log_recede_ptr(new_tail, size + sizeof(int), RAFT_LOGSIZE);
    }

    if(log->log_head != new_tail) {
      int size;
        new_tail = circular_log_recede_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
      copy_from_circular_log(log, RAFT_LOGSIZE,
			     (unsigned char *)&size, new_tail, sizeof(int)); 
      new_tail = circular_log_recede_ptr(new_tail, size + sizeof(int), RAFT_LOGSIZE);
    }

    log->log_tail = new_tail;
    pmemobj_persist(pop_raft_state,
		    &log->log_tail,
		    sizeof(unsigned long));
  }

  unsigned long read_from_log(unsigned char *dst,
			      unsigned long offset)
  {
    int size;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    const struct circular_log* log = D_RO(tmp);
    copy_from_circular_log(log,
			   RAFT_LOGSIZE,
			   (unsigned char *)&size,
			   offset,
			   sizeof(int));
    offset = circular_log_advance_ptr(offset, sizeof(int), RAFT_LOGSIZE);
    copy_from_circular_log(log, RAFT_LOGSIZE, dst, offset, size);
    offset = circular_log_advance_ptr(offset, size + sizeof(int), RAFT_LOGSIZE);
    return offset;
  }

  unsigned long read_from_log_check_size(unsigned char *dst,
					 unsigned long offset,
					 int size_check)
  {
    int size;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    const struct circular_log* log = D_RO(tmp);
    copy_from_circular_log(log,
			   RAFT_LOGSIZE,
			   (unsigned char *)&size,
			   offset,
			   sizeof(int));
    if(size != size_check) {
      BOOST_LOG_TRIVIAL(fatal) << "SIZE CHECK FAILED !";
      exit(-1);
    }
    offset = circular_log_advance_ptr(offset, sizeof(int), RAFT_LOGSIZE);
    copy_from_circular_log(log, RAFT_LOGSIZE, dst, offset, size);
    offset = circular_log_advance_ptr(offset, size + sizeof(int), RAFT_LOGSIZE);
    return offset;
  }

  unsigned long skip_log_entry(unsigned long offset)
  {
    int size;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t tmp = D_RO(root)->log;
    struct circular_log *log = D_RW(tmp);
    copy_from_circular_log(log,
			   RAFT_LOGSIZE,
			   (unsigned char *)&size,
			   offset,
			   sizeof(int));
    offset = circular_log_advance_ptr(offset, size + 2*sizeof(int), RAFT_LOGSIZE);
    return offset;
  }

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
      cyclone_tx(router->output_socket(source),
		 (unsigned char *)&resp, sizeof(msg_t), "REQVOTE RESP");
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
      rpc = pktadj2rpc(m);
      rpc->wal.leader = 0;
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
    case MSG_ASSISTED_APPENDENTRIES:
      msg->rep.ety.data.buf = msg + 1;
      router->cpaths.ring_doorbell(msg->rep.client_mc, msg->rep.client_id, msg->client_port);
      raft_recv_assisted_appendentries(raft_handle, &msg->rep);
      rte_pktmbuf_free(m);
      break;

    case MSG_ASSISTED_QUORUM_OK:
      raft_recv_assisted_quorum(raft_handle,
				&msg->rep,
				msg->quorum);
      rte_pktmbuf_free(m);
      break;
    default:
      printf("unknown msg in handle remote code=%d\n", msg->msg_type);
      exit(0);
    }
  }


  /*
  void handle_incoming_local(msg_t * msg)
  {
    int e; // TBD: need to handle errors
    char *ptr;
    msg_entry_t client_req;
    msg_entry_t *messages; 
    
    switch (msg->msg_type) {
    case MSG_CLIENT_REQ:
      client_req.id = rand();
      client_req.data.buf = msg->client.ptr;
      client_req.data.len = msg->client.size;
      client_req.type = RAFT_LOGTYPE_NORMAL;
      msg->client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
      e = raft_recv_entry(raft_handle, 
			  &client_req, 
			  msg->client_rep);
      if(e != 0) {
	free(msg->client_rep);
	msg->client_rep = NULL;
      }
      break;
    case MSG_CLIENT_REQ_BATCH:
      messages = 
	(msg_entry_t *)malloc(msg->client.size*sizeof(msg_entry_t));
      ptr = (char *)msg->client.ptr;
      for(int i=0;i<msg->client.size;i++) {
	int msg_size = msg->client.batch_sizes[i];
	messages[i].id = rand();
	messages[i].data.buf = ptr;
	messages[i].data.len = msg_size;
	ptr = ptr + msg_size;
	messages[i].type = RAFT_LOGTYPE_NORMAL;
      }
      msg->client_rep = (msg_entry_response_t *)
	malloc(msg->client.size*sizeof(msg_entry_response_t));
      e = raft_recv_entry_batch(raft_handle, 
				messages, 
				msg->client_rep,
				msg->client.size);
      if(e != 0) {
	free(msg->client_rep);
	msg->client_rep = NULL;
      }
      free(messages);
      break;
    case MSG_CLIENT_REQ_CFG:
      client_req.id = rand();
      client_req.data.buf = msg->client.ptr;
      client_req.data.len = msg->client.size;
      client_req.type = msg->client.type;
      msg->client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
      e = raft_recv_entry(raft_handle, 
			  &client_req, 
			  msg->client_rep);
      if(e != 0) {
	free(msg->client_rep);
	msg->client_rep = NULL;
      }
      break;
    case MSG_CLIENT_REQ_TERM:
      if(raft_get_current_term(raft_handle) != msg->client.term) {
	msg->client_rep = NULL;
      }
      else {
	client_req.id = rand();
	client_req.data.buf = msg->client.ptr;
	client_req.data.len = msg->client.size;
	client_req.type = RAFT_LOGTYPE_NORMAL;
	msg->client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
	e = raft_recv_entry(raft_handle, 
			    &client_req, 
			    msg->client_rep);
	if(e != 0) {
	  free(msg->client_rep);
	  msg->client_rep = NULL;
	}
      }
      break;

    case MSG_CLIENT_STATUS:
      msg->status = raft_msg_entry_response_committed
	(raft_handle, (const msg_entry_response_t *)msg->client.ptr);
      break;

    case MSG_CLIENT_REQ_SET_IMGBUILD:
      raft_set_img_build(raft_handle);
      msg->client_rep = NULL;
      break;
    case MSG_CLIENT_REQ_UNSET_IMGBUILD:
      raft_unset_img_build(raft_handle);
      msg->client_rep = NULL;
      break;
    default:
      printf("unknown msg in handle local\n");
      exit(0);
    }
    __sync_synchronize();
    msg->complete = 1;
    __sync_synchronize();
  }
  */
}cyclone_t;

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

  void operator ()()
  {
    unsigned long mark = rte_get_tsc_cycles();
    double tsc_mhz = (rte_get_tsc_hz()/1000000.0);
    unsigned long PERIODICITY_CYCLES = PERIODICITY*tsc_mhz;
    unsigned long elapsed_time;
    msg_entry_t *messages;
    rte_mbuf *pkt_array[PKT_BURST], *m;
    messages = (msg_entry_t *)malloc(PKT_BURST*sizeof(msg_entry_t));
    dpdk_socket_t *raft_socket = (dpdk_socket_t *)cyclone_handle->router->input_socket();
    dpdk_socket_t *socket = (dpdk_socket_t *)cyclone_handle->router->disp_input_socket();
    int available;
    while(!terminate) {
      // Handle any outstanding requests
      available = rte_eth_rx_burst(raft_socket->port_id,
				   raft_socket->queue_id,
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
      available = rte_eth_rx_burst(socket->port_id,
				   socket->queue_id,
				   &pkt_array[0],
				   PKT_BURST);
      if(available > 0) {
	int accepted = 0;
	for(int i=0;i<available;i++) {
	  m = pkt_array[i];
	  if(bad(m)) {
	    rte_pktmbuf_free(m);
	    continue;
	  }
	  adjust_head(m);
	  rpc_t *rpc = pktadj2rpc(m);
	  int core = rpc->client_id % executor_threads;
	  rpc->wal.leader = 1;
	  if(rpc->code == RPC_REQ_LAST_TXID || 
	     rpc->code == RPC_REQ_STATUS ||  
	     rpc->flags & RPC_FLAG_RO) {
       	    if(cyclone_is_leader(cyclone_handle)) {
	      if(rte_ring_sp_enqueue(to_cores[core], m) == -ENOBUFS) {
		BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
		exit(-1);
	      }
	      if(rte_ring_sp_enqueue(to_cores[core], rpc) == -ENOBUFS) {
		BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
		exit(-1);
	      }
	    }
	    else {
	      rte_pktmbuf_free(m);
	    }
	    continue;
	  }
	  
	  if(accepted > 0 && 
	     (messages[accepted -1].data.len + pktadj2rpcsz(m)) <= MSG_MAXSIZE) {
	    rte_mbuf *mprev = (rte_mbuf *)messages[accepted - 1].data.buf;
	    messages[accepted - 1].data.len += pktadj2rpcsz(m);
	    // Wipe out the hdr in the chained packet
	    del_adj_header(m);
	    // Chain to prev packet
	    rte_pktmbuf_chain(mprev, m);
	    //compact(mprev); // debug
	  }
	  else {
	    messages[accepted].data.buf = (void *)m;
	    messages[accepted].data.len = pktadj2rpcsz(m);
	    messages[accepted].type = RAFT_LOGTYPE_NORMAL;
	    accepted++;
	  }
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
      }
      /*
      msg_t *work = cyclone_handle->comm.get_from_runqueue();
      if(work != NULL) {
	cyclone_handle->handle_incoming_local(work);
      }
      */
      // Handle periodic events -- - AFTER any incoming requests
      elapsed_time = rte_get_tsc_cycles() - mark;
      if(elapsed_time  >= PERIODICITY_CYCLES) {
	raft_periodic(cyclone_handle->raft_handle, (int)(elapsed_time/tsc_mhz));
	mark = rte_get_tsc_cycles();
      }
    }
  }
};

#endif
