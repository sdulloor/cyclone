#ifndef _CYCLONE_CONTEXT_HPP_
#define _CYCLONE_CONTEXT_HPP_

#include <string>
#include <libpmemobj.h>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/log/trivial.hpp>
#include <zmq.h>
extern "C" {
#include <raft.h>
}
#include <boost/thread.hpp>
#include <boost/asio/io_service.hpp>
#include <boost/bind.hpp>
#include <boost/lockfree/queue.hpp>
#include "pmem_layout.h"
#include "circular_log.h"
#include "clock.hpp"


/* Message types */
const int  MSG_REQUESTVOTE              = 1;
const int  MSG_REQUESTVOTE_RESPONSE     = 2;
const int  MSG_APPENDENTRIES            = 3;
const int  MSG_APPENDENTRIES_RESPONSE   = 4;
const int  MSG_CLIENT_REQ               = 5;
const int  MSG_CLIENT_STATUS            = 6;

/* Timer */
const unsigned long PERIODICITY_MSEC    = 10UL;        

/* Cyclone max message size */
const int MSG_MAXSIZE  = 4194304;


/* Message format */
typedef struct client_io_st {
  void *ptr;
  int size;
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


static void do_zmq_send(void *socket,
			unsigned char *data,
			unsigned long size,
			const char *context) {
  while (true) {
    int rc = zmq_send(socket, data, size, 0);
    if (rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(warning) << "CYCLONE: Unable to transmit";
	perror(context);
	//Communcation failures are acceptable
      }
      // Retry
    }
    else {
      break;
    }
  }
}

static unsigned long do_zmq_recv(void *socket,
				 unsigned char *data,
				 unsigned long size,
				 const char *context)
{
  int rc;
  while (true) {
    rc = zmq_recv(socket, data, size, 0);
    if (rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(fatal) << "CYCLONE: Unable to receive";
	perror(context);
	exit(-1);
      }
      // Retry
    }
    else {
      break;
    }
  }
  return (unsigned long) rc;
}

struct cyclone_monitor;
typedef struct cyclone_st {
  boost::property_tree::ptree pt;
  void *zmq_context;
  void** zmq_push_sockets;
  void** zmq_pull_sockets;
  int replicas;
  int me;
  boost::asio::io_service ioService;
  boost::asio::io_service::work work;
  boost::thread_group threadpool;
  boost::thread *monitor_thread;
  unsigned long RAFT_LOGSIZE;
  PMEMobjpool *pop_raft_state;
  raft_server_t *raft_handle;
  cyclone_callback_t cyclone_cb;
  void *user_arg;
  unsigned char* cyclone_buffer_out;
  unsigned char* cyclone_buffer_in;
  cyclone_monitor *monitor_obj;

  cyclone_st()
    :work(ioService)
  {}

  int append_to_raft_log(unsigned char *data, int size)
  {
    int status = 0;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    TX_BEGIN(pop_raft_state){
      TX_ADD(log);
      unsigned long space_needed = size + 2*sizeof(int);
      unsigned long space_available;
      if(D_RO(log)->log_head < D_RO(log)->log_tail) {
	space_available = RAFT_LOGSIZE -
	  (D_RO(log)->log_tail - D_RO(log)->log_head);
      }
      else {
	space_available =	D_RO(log)->log_head - D_RO(log)->log_tail;
      }
      if(space_available < space_needed) {
	// Overflow !
	BOOST_LOG_TRIVIAL(fatal) << "Out of RAFT logspace !";
	pmemobj_tx_abort(-1);
      }
      unsigned long new_tail = D_RO(log)->log_tail;
      copy_to_circular_log(log,
			   RAFT_LOGSIZE,
			   D_RO(log)->log_tail,
			 (unsigned char *)&size,
			   sizeof(int));
      new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
      copy_to_circular_log(log,
			   RAFT_LOGSIZE,
			   new_tail,
			   data,
			   size);
      new_tail = circular_log_advance_ptr(new_tail, size, RAFT_LOGSIZE);
      copy_to_circular_log(log,
			   RAFT_LOGSIZE,
			   new_tail,
			   (unsigned char *)&size,
			   sizeof(int));
      new_tail = circular_log_advance_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
      D_RW(log)->log_tail = new_tail;
    } TX_ONABORT {
      status = -1;
    } TX_END
   return status;
  }

  int remove_head_raft_log()
  {
    int result = 0;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    TX_BEGIN(pop_raft_state){
      log_t log = D_RO(root)->log;
      TX_ADD(log);
      if(D_RO(log)->log_head != D_RO(log)->log_tail) {
	int size;
	copy_from_circular_log(log,
			       RAFT_LOGSIZE,
			     (unsigned char *)&size,
			       D_RO(log)->log_head,
			       sizeof(int)); 
	D_RW(log)->log_head = circular_log_advance_ptr
	  (D_RO(log)->log_head, 2*sizeof(int) + size, RAFT_LOGSIZE);
      }
    } TX_ONABORT {
      result = -1;
    } TX_END
    return result;
  }

  int remove_tail_raft_log()
  {
    int result = 0;
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    TX_BEGIN(pop_raft_state){
      log_t log = D_RO(root)->log;
      TX_ADD(log);
      if(D_RO(log)->log_head != D_RO(log)->log_tail) {
	int size;
	unsigned long new_tail = D_RO(log)->log_tail;
	new_tail = circular_log_recede_ptr(new_tail, sizeof(int), RAFT_LOGSIZE);
	copy_from_circular_log(log, RAFT_LOGSIZE,
			       (unsigned char *)&size, new_tail, sizeof(int)); 
	new_tail = circular_log_recede_ptr(new_tail, size + sizeof(int), RAFT_LOGSIZE);
	D_RW(log)->log_tail = new_tail;
      }
    } TX_ONABORT {
      result = -1;
    } TX_END
	return result;
  }

  int read_from_log(log_t log,
		    unsigned char *dst,
		    int offset)
  {
    int size;
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
  
  /* Handle incoming message and send appropriate response */
  void handle_incoming(void *socket)
  {
    unsigned long size = do_zmq_recv(socket,
				     cyclone_buffer_in,
				     MSG_MAXSIZE,
				     "Incoming");
    msg_t *msg = (msg_t *)cyclone_buffer_in;
    msg_t resp;
    unsigned long rep;
    unsigned char *payload     = cyclone_buffer_in + sizeof(msg_t);
    unsigned long payload_size = size - sizeof(msg_t); 
    int e; // TBD: need to handle errors
    char *ptr;
    msg_entry_t client_req;
    msg_entry_response_t *client_rep;
    
    switch (msg->msg_type) {
    case MSG_REQUESTVOTE:
      resp.msg_type = MSG_REQUESTVOTE_RESPONSE;
      e = raft_recv_requestvote(raft_handle, msg->source, &msg->rv, &resp.rvr);
      /* send response */
      resp.source = me;
      do_zmq_send(zmq_push_sockets[msg->source],
		  (unsigned char *)&resp, sizeof(msg_t), "REQVOTE RESP");
      break;
    case MSG_REQUESTVOTE_RESPONSE:
      e = raft_recv_requestvote_response(raft_handle, msg->source, &msg->rvr);
      break;
    case MSG_APPENDENTRIES:
    resp.msg_type = MSG_APPENDENTRIES_RESPONSE;
    msg->ae.entries = (msg_entry_t *)payload;
    ptr = (char *)(payload + msg->ae.n_entries*sizeof(msg_entry_t));
    for(int i=0;i<msg->ae.n_entries;i++) {
      msg->ae.entries[i].data.buf = malloc(msg->ae.entries[i].data.len);
      memcpy(msg->ae.entries[i].data.buf, 
	     ptr, 
	     msg->ae.entries[i].data.len);
      ptr += msg->ae.entries[i].data.len;
    }
    e = raft_recv_appendentries(raft_handle, msg->source, &msg->ae, &resp.aer);
    resp.source = me;
    do_zmq_send(zmq_push_sockets[msg->source],
		(unsigned char *)&resp, sizeof(msg_t), "APPENDENTRIES RESP");
    break;
    case MSG_APPENDENTRIES_RESPONSE:
      e = raft_recv_appendentries_response(raft_handle, msg->source, &msg->aer);
      break;
    case MSG_CLIENT_REQ:
      if(!cyclone_is_leader(this)) {
	client_rep = NULL;
	do_zmq_send(zmq_pull_sockets[me],
		    (unsigned char *)&client_rep,
		    sizeof(void *),
		    "CLIENT COOKIE SEND");
      }
      else {
	client_req.id = rand();
	client_req.data.buf = msg->client.ptr;
	client_req.data.len = msg->client.size;
	// TBD: Handle error
	client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
	(void)raft_recv_entry(raft_handle, me, &client_req, client_rep);
	do_zmq_send(zmq_pull_sockets[me],
		    (unsigned char *)&client_rep,
		    sizeof(void *),
		    "CLIENT COOKIE SEND");
      }
      break;
    case MSG_CLIENT_STATUS:
      e = raft_msg_entry_response_committed
	(raft_handle, (const msg_entry_response_t *)msg->client.ptr);
      do_zmq_send(zmq_pull_sockets[me],
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


#endif
