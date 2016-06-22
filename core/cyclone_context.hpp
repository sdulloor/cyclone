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


/* Cyclone max message size */
const int MSG_MAXSIZE  = 4194304;


/* Message format */
typedef struct client_io_st {
  void *ptr;
  int size;
  int term;
  int type;
  int* batch_sizes;
} client_t;

typedef struct
{
  int msg_type;
  int source;
  union
  {
    msg_requestvote_t rv;
    msg_requestvote_response_t rvr;
    msg_appendentries_t ae;
    msg_appendentries_response_t aer;
    client_t client;
  };
} msg_t;

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
  cyclone_callback_t cyclone_rep_cb;
  cyclone_callback_t cyclone_pop_cb;
  cyclone_callback_t cyclone_commit_cb;
  void *user_arg;
  unsigned char* cyclone_buffer_out;
  unsigned char* cyclone_buffer_in;
  cyclone_monitor *monitor_obj;

  cyclone_st()
  {}

  unsigned long get_log_offset()
  {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    return D_RO(log)->log_tail;
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
    pmemobj_persist(pop_raft_state,
		    &log->log_head,
		    sizeof(unsigned long));
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
  void handle_incoming(unsigned long size)
  {
    msg_t *msg = (msg_t *)cyclone_buffer_in;
    msg_t resp;
    unsigned long rep;
    unsigned char *payload     = cyclone_buffer_in + sizeof(msg_t);
    unsigned long payload_size = size - sizeof(msg_t); 
    int e; // TBD: need to handle errors
    char *ptr;
    msg_entry_t client_req;
    msg_entry_response_t *client_rep;
    msg_entry_t *messages; 
    
    switch (msg->msg_type) {
    case MSG_REQUESTVOTE:
      resp.msg_type = MSG_REQUESTVOTE_RESPONSE;
      e = raft_recv_requestvote(raft_handle, 
				raft_get_node(raft_handle, msg->source), 
				&msg->rv, 
				&resp.rvr);
      /* send response */
      resp.source = me;
      cyclone_tx(router->output_socket(msg->source),
		 (unsigned char *)&resp, sizeof(msg_t), "REQVOTE RESP");
      break;
    case MSG_REQUESTVOTE_RESPONSE:
      e = raft_recv_requestvote_response(raft_handle, 
					 raft_get_node(raft_handle, msg->source), 
					 &msg->rvr);
      break;
    case MSG_APPENDENTRIES:
    resp.msg_type = MSG_APPENDENTRIES_RESPONSE;
    msg->ae.entries = (msg_entry_t *)payload;
    ptr = (char *)(payload + msg->ae.n_entries*sizeof(msg_entry_t));
    for(int i=0;i<msg->ae.n_entries;i++) {
      msg->ae.entries[i].data.buf = ptr;
      ptr += msg->ae.entries[i].data.len;
    }
    e = raft_recv_appendentries(raft_handle, 
				raft_get_node(raft_handle, msg->source), 
				&msg->ae, 
				&resp.aer);
    resp.source = me;
    cyclone_tx(router->output_socket(msg->source),
		(unsigned char *)&resp, sizeof(msg_t), "APPENDENTRIES RESP");
    break;
    case MSG_APPENDENTRIES_RESPONSE:
      e = raft_recv_appendentries_response(raft_handle, 
					   raft_get_node(raft_handle, msg->source), 
					   &msg->aer);
      break;
    case MSG_CLIENT_REQ:
      client_req.id = rand();
      client_req.data.buf = msg->client.ptr;
      client_req.data.len = msg->client.size;
      client_req.type = RAFT_LOGTYPE_NORMAL;
      client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
      e = raft_recv_entry(raft_handle, 
			  &client_req, 
			  client_rep);
      if(e != 0) {
	free(client_rep);
	client_rep = NULL;
      }
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&client_rep,
		       sizeof(void *),
		       "CLIENT COOKIE SEND");
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
      client_rep = (msg_entry_response_t *)
	malloc(msg->client.size*sizeof(msg_entry_response_t));
      e = raft_recv_entry_batch(raft_handle, 
				messages, 
				client_rep,
				msg->client.size);
      if(e != 0) {
	free(client_rep);
	client_rep = NULL;
      }
      free(messages);
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&client_rep,
		       sizeof(void *),
		       "CLIENT COOKIE SEND");
      break;
    case MSG_CLIENT_REQ_CFG:
      client_req.id = rand();
      client_req.data.buf = msg->client.ptr;
      client_req.data.len = msg->client.size;
      client_req.type = msg->client.type;
      client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
      e = raft_recv_entry(raft_handle, 
			  &client_req, 
			  client_rep);
      if(e != 0) {
	free(client_rep);
	client_rep = NULL;
      }
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&client_rep,
		       sizeof(void *),
		       "CLIENT COOKIE SEND");
      break;
    case MSG_CLIENT_REQ_TERM:
      if(raft_get_current_term(raft_handle) != msg->client.term) {
	client_rep = NULL;
	cyclone_tx_block(router->request_in(),
			 (unsigned char *)&client_rep,
			 sizeof(void *),
			 "CLIENT COOKIE SEND");
      }
      else {
	client_req.id = rand();
	client_req.data.buf = msg->client.ptr;
	client_req.data.len = msg->client.size;
	client_req.type = RAFT_LOGTYPE_NORMAL;
	client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
	e = raft_recv_entry(raft_handle, 
			      &client_req, 
			      client_rep);
	if(e != 0) {
	  free(client_rep);
	  client_rep = NULL;
	}
	cyclone_tx_block(router->request_in(),
			 (unsigned char *)&client_rep,
			 sizeof(void *),
			 "CLIENT COOKIE SEND");
      }
      break;

    case MSG_CLIENT_STATUS:
      e = raft_msg_entry_response_committed
	(raft_handle, (const msg_entry_response_t *)msg->client.ptr);
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&e,
		       sizeof(int),
		       "CLIENT COOKIE SEND");
      break;

    case MSG_CLIENT_REQ_SET_IMGBUILD:
      raft_set_img_build(raft_handle);
      client_rep = NULL;
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&e,
		       sizeof(int),
		       "CLIENT COOKIE SEND");
      break;
    case MSG_CLIENT_REQ_UNSET_IMGBUILD:
      raft_unset_img_build(raft_handle);
      client_rep = NULL;
      cyclone_tx_block(router->request_in(),
		       (unsigned char *)&e,
		       sizeof(int),
		       "CLIENT COOKIE SEND");
      break;
    default:
      printf("unknown msg\n");
      exit(0);
    }
  }
}cyclone_t;


struct cyclone_monitor {
  volatile bool terminate;
  cyclone_t *cyclone_handle;
  void *poll_items;

  cyclone_monitor()
  :terminate(false)
  {}
  
  void operator ()()
  {
    unsigned long mark = rtc_clock::current_time();
    unsigned long elapsed_time;
    while(!terminate) {
      // Handle any outstanding requests
      for(int i=0;i<cyclone_handle->replicas;i++) {
	int sz =
	  cyclone_rx(cyclone_handle->router->input_socket(),
		     cyclone_handle->cyclone_buffer_in,
		     MSG_MAXSIZE,
		     "Incoming");
	if(sz != -1) {
	  cyclone_handle->handle_incoming(sz);
	}
	sz =
	  cyclone_rx(cyclone_handle->router->request_in(),
		     cyclone_handle->cyclone_buffer_in,
		     MSG_MAXSIZE,
		     "Incoming");
	if(sz != -1) {
	  cyclone_handle->handle_incoming(sz);
	}
      }
      // Handle periodic events -- - AFTER any incoming requests
      if((elapsed_time = rtc_clock::current_time() - mark) >= PERIODICITY) {
	raft_periodic(cyclone_handle->raft_handle, (int)elapsed_time);
	mark = rtc_clock::current_time();
      }
    }
  }
};

#endif
