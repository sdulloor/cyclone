// Asynchronous fault tolerant pmem log replication with cyclone
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
#include "libcyclone.hpp"



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


/** Raft callback for sending request vote message */
static int __send_requestvote(raft_server_t* raft,
			      void *user_data,
			      int nodeidx,
			      msg_requestvote_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)user_data;
  raft_node_t* node = raft_get_node(raft, nodeidx);
  void *socket      = raft_node_get_udata(node);
  msg_t msg;
  msg.source      = cyclone_handle->me;
  msg.msg_type    = MSG_REQUESTVOTE;
  msg.rv          = *m;
  do_zmq_send(socket, 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "__send_requestvote");
  return 0;
}

/** Raft callback for sending appendentries message */
static int __send_appendentries(raft_server_t* raft,
				void *udata,
				int nodeidx,
				msg_appendentries_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  raft_node_t* node = raft_get_node(raft, nodeidx);
  void *socket      = raft_node_get_udata(node);
  unsigned char *ptr = cyclone_handle->cyclone_buffer_out;
  msg_t msg;
  msg.msg_type         = MSG_APPENDENTRIES;
  msg.source           = cyclone_handle->me;
  msg.ae.term          = m->term;
  msg.ae.prev_log_idx  = m->prev_log_idx;
  msg.ae.prev_log_term = m->prev_log_term;
  msg.ae.leader_commit = m->leader_commit;
  msg.ae.n_entries     = m->n_entries;
  memcpy(ptr, &msg, sizeof(msg));
  ptr = ptr + sizeof(msg);
  for(int i=0;i<m->n_entries;i++) {
    memcpy(ptr, &m->entries[i], sizeof(msg_entry_t));
    ptr += sizeof(msg_entry_t);
  }
  for(int i=0;i<m->n_entries;i++) {
    memcpy(ptr, m->entries[i].data.buf, m->entries[i].data.len);
    ptr += m->entries[i].data.len;
  }
  do_zmq_send(socket, 
	      cyclone_handle->cyclone_buffer_out, 
	      ptr - cyclone_handle->cyclone_buffer_out, 
	      "__send_requestvote");
  return 0;
}



/** Raft callback for saving voted_for field to disk.
 * This only returns when change has been made to disk. */
static int __persist_vote(raft_server_t* raft,
			  void *udata,
			  const int voted_for)
{
  int status = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state,
				       raft_pstate_t);
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->voted_for = voted_for;
  }TX_ONABORT {
    status = -1;
  } TX_END
  return status;
}


/** Raft callback for saving term field to disk.
 * This only returns when change has been made to disk. */
static int __persist_term(raft_server_t* raft,
			  void *udata,
			  const int current_term)
{
  int status = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state,
				       raft_pstate_t);
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->term = current_term;
  } TX_ONABORT {
    status = -1;
  } TX_END
  return status;
}

static int __applylog(raft_server_t* raft,
		      void *udata,
		      const unsigned char *data,
		      const int len)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  cyclone_handle->cyclone_cb(cyclone_handle->user_arg, data, len);
  return 0;
}


/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(raft_server_t* raft,
				 void *udata,
				 raft_entry_t *ety,
				 int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety,
					  sizeof(raft_entry_t)) != 0) {
      pmemobj_tx_abort(-1);
    }
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety->data.buf,
					  ety->data.len) != 0) {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END

  return result;
}


/** Raft callback for removing the first entry from the log
 * @note this is provided to support log compaction in the future */
static int __raft_logentry_poll(raft_server_t* raft,
				void *udata,
				raft_entry_t *entry,
				int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->remove_head_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(cyclone_handle->remove_head_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

/** Raft callback for deleting the most recent entry from the log.
 * This happens when an invalid leader finds a valid leader and has to delete
 * superseded log entries. */
static int __raft_logentry_pop(raft_server_t* raft,
			       void *udata,
			       raft_entry_t *entry,
			       int ety_idx)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    if(cyclone_handle->remove_tail_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(cyclone_handle->remove_tail_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t* raft, void *udata, const char *buf)
{
  //BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT " << buf;
}



raft_cbs_t raft_funcs = {
  .send_requestvote            = __send_requestvote,
  .send_appendentries          = __send_appendentries,
  .applylog                    = __applylog,
  .persist_vote                = __persist_vote,
  .persist_term                = __persist_term,
  .log_offer                   = __raft_logentry_offer,
  .log_poll                    = __raft_logentry_poll,
  .log_pop                     = __raft_logentry_pop,
  .log                         = __raft_log,
};


static void cyclone_bind_endpoint(void *socket, const char *endpoint)
{
  int rc = zmq_bind(socket, endpoint);
  if (rc != 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "CYCLONE::COMM Unable to setup listening socket at "
      << endpoint;
    perror("zmq_bind:");
    exit(-1);
    }
  else {
    BOOST_LOG_TRIVIAL(info)
      << "CYCLONE::COMM Listening at "
      << endpoint;
  }
}

static void cyclone_connect_endpoint(void *socket, const char *endpoint)
{
  BOOST_LOG_TRIVIAL(info)
    << "CYCLONE::COMM Connecting to "
    << endpoint;
  zmq_connect(socket, endpoint);
}



struct cyclone_monitor {
  volatile bool terminate;
  cyclone_t *cyclone_handle;
  cyclone_monitor()
  :terminate(false)
  {}
  
  void operator ()()
  {
    rtc_clock timer;
    zmq_pollitem_t *items = new zmq_pollitem_t[cyclone_handle->replicas];
    for(int i=0;i<cyclone_handle->replicas;i++) {
      items[i].socket = cyclone_handle->zmq_pull_sockets[i];
      items[i].events = ZMQ_POLLIN;
    }
    while(!terminate) {
      timer.start();
      int e = zmq_poll(items, cyclone_handle->replicas, PERIODICITY_MSEC);
      timer.stop();
      // Handle any outstanding requests
      for(int i=0;i<=cyclone_handle->replicas;i++) {
	if(items[i].revents & ZMQ_POLLIN) {
	  cyclone_handle->handle_incoming(cyclone_handle->zmq_pull_sockets[i]);
	}
      }
      // Handle periodic events -- - AFTER any incoming requests
      if(timer.elapsed_time()/1000 >= PERIODICITY_MSEC) {
	raft_periodic(cyclone_handle->raft_handle, timer.elapsed_time()/1000);
	timer.reset();
      }
    }
  }
};

int cyclone_is_leader(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  int leader = raft_get_current_leader(handle->raft_handle);
  return (leader == handle->me) ? 1:0;
}

void* cyclone_add_entry(void *cyclone_handle, void *data, int size)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ;
  msg.client.ptr  = data;
  msg.client.size = size;
  do_zmq_send(handle->zmq_push_sockets[handle->me], 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "client req");
  do_zmq_recv(handle->zmq_push_sockets[handle->me],
	      (unsigned char *)&cookie,
	      sizeof(void *),
	      "CLIENT REQ recv");
  return cookie;
}

int cyclone_check_status(void *cyclone_handle, void *cookie)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_STATUS;
  msg.client.ptr  = cookie;
  int result;
  do_zmq_send(handle->zmq_push_sockets[handle->me], 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "client status");
  do_zmq_recv(handle->zmq_push_sockets[handle->me],
	      (unsigned char *)&result,
	      sizeof(int),
	      "CLIENT STATUS RECV");
  return result;
}

static void init_log(PMEMobjpool *pop, void *ptr, void *arg)
{
  struct circular_log *log = (struct circular_log *)ptr;
  log->log_head  = 0;
  log->log_tail  = 0;
}

void* cyclone_boot(const char *config_path, cyclone_callback_t cyclone_callback, void *user_arg)
{
  cyclone_t *cyclone_handle;
  std::stringstream key;
  std::stringstream addr;

  cyclone_handle = new cyclone_t();
  cyclone_handle->cyclone_cb = cyclone_callback;
  cyclone_handle->user_arg   = user_arg;
  
  boost::property_tree::read_ini(config_path, cyclone_handle->pt);
  std::string path_raft           = cyclone_handle->pt.get<std::string>("storage.raftpath");
  cyclone_handle->RAFT_LOGSIZE    = cyclone_handle->pt.get<unsigned long>("storage.logsize");
  cyclone_handle->replicas        = cyclone_handle->pt.get<int>("network.replicas");
  cyclone_handle->me              = cyclone_handle->pt.get<int>("network.me");
  unsigned long baseport  = cyclone_handle->pt.get<unsigned long>("network.baseport"); 
  
  cyclone_handle->raft_handle = raft_new();
  raft_set_callbacks(cyclone_handle->raft_handle, &raft_funcs, cyclone_handle);
  raft_set_election_timeout(cyclone_handle->raft_handle, 1000);
  //raft_set_request_timeout(cyclone_handle->raft_handle, 200);

  /* setup connections */
  cyclone_handle->zmq_context  = zmq_init(1); // One thread should be enough ?
  cyclone_handle->zmq_pull_sockets = new void*[cyclone_handle->replicas];
  cyclone_handle->zmq_push_sockets = new void*[cyclone_handle->replicas];

  // PUSH sockets
  for(int i=0;i<cyclone_handle->replicas;i++) {
    if(i != cyclone_handle->me) {
      cyclone_handle->zmq_push_sockets[i] = zmq_socket(cyclone_handle->zmq_context, ZMQ_PUSH);
    }
    else {
      cyclone_handle->zmq_push_sockets[i] = zmq_socket(cyclone_handle->zmq_context, ZMQ_REQ);
    }
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.addr" << i;
    addr << "tcp://";
    addr << cyclone_handle->pt.get<std::string>(key.str().c_str());
    unsigned long port = baseport + i*cyclone_handle->replicas + cyclone_handle->me;
    addr << ":" << port;
    cyclone_connect_endpoint(cyclone_handle->zmq_push_sockets[i], addr.str().c_str());
    raft_add_peer(cyclone_handle->raft_handle,
		  cyclone_handle->zmq_push_sockets[i],
		  i == cyclone_handle->me ? 1:0);
  }

  // PULL sockets
  for(int i=0;i<cyclone_handle->replicas;i++) {
    if(i != cyclone_handle->me) {
      cyclone_handle->zmq_pull_sockets[i] = zmq_socket(cyclone_handle->zmq_context, ZMQ_PULL);
    }
    else {
      cyclone_handle->zmq_pull_sockets[i] = zmq_socket(cyclone_handle->zmq_context, ZMQ_REP);
    }
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.iface" << i;
    addr << "tcp://";
    addr << cyclone_handle->pt.get<std::string>(key.str().c_str());
    unsigned long port = baseport + cyclone_handle->me*cyclone_handle->replicas + i;
    addr << ":" << port;
    cyclone_bind_endpoint(cyclone_handle->zmq_pull_sockets[i], addr.str().c_str());
  }
  
  /* Setup raft state */
  cyclone_handle->pop_raft_state = pmemobj_open(path_raft.c_str(),
						"raft_persistent_state");
  if (cyclone_handle->pop_raft_state == NULL) {
    // TBD: figure out how to make this atomic
    cyclone_handle->pop_raft_state = pmemobj_create(path_raft.c_str(),
						    POBJ_LAYOUT_NAME(raft_persistent_state),
						    PMEMOBJ_MIN_POOL,
						    0666);
    if(cyclone_handle->pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    D_RW(root)->term      = 0;
    D_RW(root)->voted_for = -1;
    log_t log;
    POBJ_ALLOC(cyclone_handle->pop_raft_state,
	       &log,
	       struct circular_log,
	       sizeof(struct circular_log) + cyclone_handle->RAFT_LOGSIZE,
	       init_log,
	       NULL);
    if(TOID_IS_NULL(log)) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to allocate log:"
	<< strerror(errno);
      exit(-1);
    }
    D_RW(root)->log      = log; 
  }
  else {
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    raft_vote(cyclone_handle->raft_handle, D_RO(root)->voted_for);
    raft_set_current_term(cyclone_handle->raft_handle, D_RO(root)->term);
    unsigned long ptr = D_RO(log)->log_head;
    raft_entry_t ety;
    while(ptr != D_RO(log)->log_tail) {
      ptr = cyclone_handle->read_from_log(log, (unsigned char *)&ety, ptr);
      ety.data.buf = malloc(ety.data.len);
      ptr = cyclone_handle->read_from_log(log, (unsigned char *)ety.data.buf, ptr);
      raft_append_entry(cyclone_handle->raft_handle, &ety);
    }
  }
  cyclone_handle->cyclone_buffer_in  = new unsigned char[MSG_MAXSIZE];
  cyclone_handle->cyclone_buffer_out = new unsigned char[MSG_MAXSIZE];
  /* Launch cyclone service */
  cyclone_handle->threadpool.create_thread(boost::bind(&boost::asio::io_service::run,
						       &cyclone_handle->ioService));
  cyclone_handle->monitor_obj    = new cyclone_monitor();
  cyclone_handle->monitor_obj->cyclone_handle    = cyclone_handle;
  cyclone_handle->monitor_thread = new boost::thread(boost::ref(*cyclone_handle->monitor_obj));
  return cyclone_handle;
}

void cyclone_shutdown(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  handle->ioService.stop();
  handle->threadpool.join_all();
  handle->monitor_obj->terminate = true;
  handle->monitor_thread->join();
  delete handle->monitor_obj;
  for(int i=0;i<=handle->replicas;i++) {
    zmq_close(handle->zmq_push_sockets[i]);
    zmq_close(handle->zmq_pull_sockets[i]);
  }
  zmq_ctx_destroy(handle->zmq_context);
  delete[] handle->zmq_push_sockets;
  delete[] handle->zmq_pull_sockets;
  pmemobj_close(handle->pop_raft_state);
  delete[] handle->cyclone_buffer_in;
  delete[] handle->cyclone_buffer_out;
  delete handle;
}
