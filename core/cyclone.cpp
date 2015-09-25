// Asynchronous fault tolerant pmem log replication with cyclone
#include <string>
#include <libpmemlog.h>
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
#include "clock.hpp"
#include "libcyclone.hpp"

static boost::property_tree::ptree pt;
static void** zmq_push_sockets;
static void** zmq_pull_sockets;
static int replicas;
static int me;

static boost::asio::io_service ioService;
static boost::asio::io_service::work work(ioService);
static boost::thread_group threadpool;
static boost::thread *monitor_thread;

/* size of the RAFT log */
unsigned long RAFT_LOGSIZE;

/* Message types */
const int  MSG_REQUESTVOTE              = 1;
const int  MSG_REQUESTVOTE_RESPONSE     = 2;
const int  MSG_APPENDENTRIES            = 3;
const int  MSG_APPENDENTRIES_RESPONSE   = 4;
const int  MSG_CLIENT_REQ               = 5;

const unsigned long PERIODICITY_MSEC    = 500UL;        


/* Cyclone max message size */
const int MSG_MAXSIZE  = 4194304;
static unsigned char cyclone_buffer_out[MSG_MAXSIZE];
static unsigned char cyclone_buffer_in[MSG_MAXSIZE];
static raft_server_t *raft_handle;
/* Outstanding client request */
static msg_entry_t client_req_entry;
static msg_entry_response_t client_req_resp;
static cyclone_req_t * volatile client_req;



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
  };
} msg_t;

/* Persistent RAFT state */
struct circular_log {
  unsigned long log_head;
  unsigned long log_tail;
  unsigned char data[0];
};

POBJ_LAYOUT_BEGIN(raft_persistent_state);
POBJ_LAYOUT_TOID(raft_persistent_state, struct circular_log)
typedef struct raft_pstate_st {
  int term;
  int voted_for;
  TOID(struct circular_log) log;
} raft_pstate_t;
POBJ_LAYOUT_ROOT(raft_persistent_state, raft_pstate_t);
POBJ_LAYOUT_END(raft_persistent_state);

static PMEMobjpool *pop_raft_state;
cyclone_callback_t cyclone_cb;

void CIRCULAR_COPY_FROM_LOG(unsigned char *dst,
			    unsigned long offset,
			    unsigned long size)
{
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TOID(struct circular_log) log = D_RW(root)->log;
  unsigned long chunk1 = (offset + size) > RAFT_LOGSIZE ?
    (RAFT_LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  TX_MEMCPY(dst, D_RO(log)->data + offset, chunk1);
  if(chunk2 > 0) {
    dst += chunk1;
    TX_MEMCPY(dst, D_RO(log)->data, chunk2);
  }
}

void CIRCULAR_COPY_TO_LOG(unsigned long offset,
			  unsigned char *src,
			  unsigned long size)
{
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TOID(struct circular_log) log = D_RW(root)->log;
  unsigned long chunk1 = (offset + size) > RAFT_LOGSIZE ?
    (RAFT_LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  TX_MEMCPY(D_RW(log)->data + offset, src, chunk1);
  if(chunk2 > 0) {
    src += chunk1;
    TX_MEMCPY(D_RW(log)->data, src, chunk2);
  }
}

unsigned long ADVANCE_LOG_PTR(unsigned long ptr, unsigned long size)
{
  ptr = ptr + size;
  if(ptr > RAFT_LOGSIZE) {
    ptr = ptr - RAFT_LOGSIZE;
  }
  return ptr;
}

unsigned long RECEDE_LOG_PTR(unsigned long ptr, unsigned long size)
{
  if(ptr < size) {
    ptr = RAFT_LOGSIZE - (size - ptr);
  }
  else {
    ptr = ptr - size;
  }
  return ptr;
}

int append_to_raft_log(unsigned char *data, int size)
{
  int status = 0;
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TX_BEGIN(pop_raft_state){
    TOID(struct circular_log) log = D_RW(root)->log;
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
    CIRCULAR_COPY_TO_LOG(D_RO(log)->log_tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = ADVANCE_LOG_PTR(new_tail, sizeof(int));
    CIRCULAR_COPY_TO_LOG(new_tail,
			 data,
			 size);
    new_tail = ADVANCE_LOG_PTR(new_tail, size);
    CIRCULAR_COPY_TO_LOG(new_tail,
			 (unsigned char *)&size,
			 sizeof(int));
    new_tail = ADVANCE_LOG_PTR(new_tail, sizeof(int));
    D_RW(log)->log_tail = new_tail;
  } TX_ONABORT {
    status = -1;
  } TX_END
  return status;
}

static int remove_head_raft_log()
{
  int result = 0;
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TX_BEGIN(pop_raft_state){
    TOID(struct circular_log) log = D_RW(root)->log;
    TX_ADD(log);
    if(D_RO(log)->log_head != D_RO(log)->log_tail) {
      int size;
      CIRCULAR_COPY_FROM_LOG((unsigned char *)&size, D_RO(log)->log_head, sizeof(int)); 
      D_RW(log)->log_head = ADVANCE_LOG_PTR(D_RO(log)->log_head, 2*sizeof(int) + size);
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

static int remove_tail_raft_log()
{
  int result = 0;
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TX_BEGIN(pop_raft_state){
    TOID(struct circular_log) log = D_RW(root)->log;
    TX_ADD(log);
    if(D_RO(log)->log_head != D_RO(log)->log_tail) {
      int size;
      unsigned long new_tail = D_RO(log)->log_tail;
      new_tail = RECEDE_LOG_PTR(new_tail, sizeof(int));
      CIRCULAR_COPY_FROM_LOG((unsigned char *)&size, new_tail, sizeof(int)); 
      new_tail = RECEDE_LOG_PTR(new_tail, size + sizeof(int));
      D_RW(log)->log_tail = new_tail;
    }
  } TX_ONABORT {
    result = -1;
  } TX_END
  return result;
}

static int read_from_log(unsigned char *dst, int offset)
{
  int size;
  CIRCULAR_COPY_FROM_LOG((unsigned char *)&size, offset, sizeof(int));
  offset = ADVANCE_LOG_PTR(offset, sizeof(int));
  CIRCULAR_COPY_FROM_LOG(dst, offset, size);
  offset = ADVANCE_LOG_PTR(offset, size + sizeof(int));
  return offset;
}

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

/** Raft callback for sending request vote message */
static int __send_requestvote(raft_server_t* raft,
			      void *user_data,
			      int nodeidx,
			      msg_requestvote_t* m)
{
  raft_node_t* node = raft_get_node(raft, nodeidx);
  void *socket      = raft_node_get_udata(node);
  msg_t msg;
  msg.source          = me;
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
  raft_node_t* node = raft_get_node(raft, nodeidx);
  void *socket      = raft_node_get_udata(node);
  unsigned char *ptr = cyclone_buffer_out;
  msg_t msg;
  msg.msg_type         = MSG_APPENDENTRIES;
  msg.source           = me;
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
	      cyclone_buffer_out, 
	      ptr - cyclone_buffer_out, 
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
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TX_BEGIN(pop_raft_state) {
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
  TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
  TX_BEGIN(pop_raft_state) {
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
  cyclone_cb(data, len);
  return 0;
}


/** Raft callback for appending an item to the log */
static int __raft_logentry_offer(raft_server_t* raft,
				 void *udata,
				 raft_entry_t *ety,
				 int ety_idx)
{
  int result = 0;
  TX_BEGIN(pop_raft_state) {
    if(append_to_raft_log((unsigned char *)ety, sizeof(raft_entry_t)) != 0) {
      pmemobj_tx_abort(-1);
    }
    if(append_to_raft_log((unsigned char *)ety->data.buf, ety->data.len) != 0) {
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
  TX_BEGIN(pop_raft_state) {
    if(remove_head_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(remove_head_raft_log() != 0) {
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
  TX_BEGIN(pop_raft_state) {
    if(remove_tail_raft_log() != 0) {
      pmemobj_tx_abort(-1);
    }
    if(remove_tail_raft_log() != 0) {
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



/* Handle incoming message and send appropriate response */
static void handle_incoming(void *socket)
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
      msg->ae.entries[i].data.buf = ptr;
      ptr += msg->ae.entries[i].data.len;
    }
    e = raft_recv_appendentries(raft_handle, msg->source, &msg->ae, &resp.aer);
    resp.source = me;
    do_zmq_send(zmq_push_sockets[msg->source],
		(unsigned char *)&resp, sizeof(msg_t), "APPENDENTRIES RESP");
    break;
  case MSG_APPENDENTRIES_RESPONSE:
    e = raft_recv_appendentries_response(raft_handle, msg->source, &msg->aer);
    if(client_req != NULL) {
      e = raft_msg_entry_response_committed(raft_handle,
					    &client_req_resp);
      if(e != 0) {
	client_req->response_code = e;
	__sync_synchronize();
	client_req->request_complete = 1;
	client_req = NULL;
      }
    }
    break;
  case MSG_CLIENT_REQ:
    if(!cyclone_is_leader()) {
      client_req->response_code = -1;
      __sync_synchronize();
      client_req->request_complete = 1;
      client_req = NULL;
    }
    else {
      client_req_entry.id = rand();
      client_req_entry.data.buf = client_req->data;
      client_req_entry.data.len = client_req->size;
      // TBD: Handle error
      (void)raft_recv_entry(raft_handle, me, &client_req_entry, &client_req_resp);
      e = raft_msg_entry_response_committed(raft_handle,
					    &client_req_resp);
      if(e != 0) {
	client_req->response_code = e;
	__sync_synchronize();
	client_req->request_complete = 1;
	client_req = NULL;
      }
    }
    break;
  default:
    printf("unknown msg\n");
    exit(0);
  }
}

struct monitor_incoming {
  volatile bool terminate;
  monitor_incoming()
    :terminate(false)
  {}
  
  void operator ()()
  {
    rtc_clock timer;
    zmq_pollitem_t *items = new zmq_pollitem_t[replicas];
    for(int i=0;i<replicas;i++) {
      items[i].socket = zmq_pull_sockets[i];
      items[i].events = ZMQ_POLLIN;
    }
    while(!terminate) {
      timer.start();
      //BOOST_LOG_TRIVIAL(info) << "Enter poll";
      int e = zmq_poll(items, replicas + 1, PERIODICITY_MSEC/2);
      timer.stop();
      // Handle periodic events
      //BOOST_LOG_TRIVIAL(info) << "Exit poll";
      if(timer.elapsed_time()/1000 >= PERIODICITY_MSEC) {
	raft_periodic(raft_handle, timer.elapsed_time());
	timer.reset();
      }
      // Handle any outstanding requests
      for(int i=0;i<=replicas;i++) {
	if(items[i].revents & ZMQ_POLLIN) {
	  handle_incoming(zmq_pull_sockets[i]);
	}
      }
    }
  }
} *monitor_obj;

int cyclone_is_leader()
{
  int leader = raft_get_current_leader(raft_handle);
  //BOOST_LOG_TRIVIAL(info) << "leader = " << leader;
  return (leader == me) ? 1:0;
}

int cyclone_add_entry(cyclone_req_t *req)
{
  msg_t msg;
  client_req = req;
  req->request_complete = 0;
  __sync_synchronize();
  msg.source      = me;
  msg.msg_type    = MSG_CLIENT_REQ;
  do_zmq_send(zmq_push_sockets[me], 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "__send_requestvote");
  return 0;
}

void init_log(PMEMobjpool *pop, void *ptr, void *arg)
{
  struct circular_log *log = (struct circular_log *)ptr;
  log->log_head  = 0;
  log->log_tail  = 0;
}

void cyclone_boot(const char *config_path, cyclone_callback_t cyclone_callback) 
{
  void *zmq_context;
  std::stringstream key;
  std::stringstream addr;

  cyclone_cb = cyclone_callback;
  
  boost::property_tree::read_ini(config_path, pt);
  std::string path_raft   = pt.get<std::string>("storage.raftpath");
  RAFT_LOGSIZE            = pt.get<unsigned long>("storage.logsize");
  replicas                = pt.get<int>("network.replicas");
  me                      = pt.get<int>("network.me");
  unsigned long baseport  = pt.get<unsigned long>("network.baseport"); 
  
  raft_handle = raft_new();
  raft_set_callbacks(raft_handle, &raft_funcs, NULL);
  

  /* setup connections */
  zmq_context  = zmq_init(1); // One thread should be enough ?
  zmq_pull_sockets = new void*[replicas];
  zmq_push_sockets = new void*[replicas];

  // PUSH sockets
  for(int i=0;i<replicas;i++) {
    zmq_push_sockets[i] = zmq_socket(zmq_context, ZMQ_PUSH);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.addr" << i;
    addr << "tcp://";
    addr << pt.get<std::string>(key.str().c_str());
    unsigned long port = baseport + i*replicas + me;
    addr << ":" << port;
    cyclone_connect_endpoint(zmq_push_sockets[i], addr.str().c_str());
    raft_add_peer(raft_handle, zmq_push_sockets[i], i == me ? 1:0);
  }

  // PULL sockets
  for(int i=0;i<replicas;i++) {
    zmq_pull_sockets[i] = zmq_socket(zmq_context, ZMQ_PULL);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.iface" << i;
    addr << "tcp://";
    addr << pt.get<std::string>(key.str().c_str());
    unsigned long port = baseport + me*replicas + i;
    addr << ":" << port;
    cyclone_bind_endpoint(zmq_pull_sockets[i], addr.str().c_str());
  }
  
  /* Setup raft state */
  pop_raft_state = pmemobj_open(path_raft.c_str(),
				"raft_persistent_state");
  if (pop_raft_state == NULL) {
    // TBD: figure out how to make this atomic
    pop_raft_state = pmemobj_create(path_raft.c_str(),
				    POBJ_LAYOUT_NAME(raft_persistent_state),
				    PMEMOBJ_MIN_POOL,
				    0666);
    if(pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    D_RW(root)->term      = 0;
    D_RW(root)->voted_for = -1;
    TOID(struct circular_log) log;
    POBJ_ALLOC(pop_raft_state, &log, struct circular_log,
	       sizeof(struct circular_log) + RAFT_LOGSIZE,
	       init_log, NULL);
    if(TOID_IS_NULL(log)) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to allocate log:"
	<< strerror(errno);
      exit(-1);
    }
    D_RW(root)->log      = log; 
  }
  else {
    TOID(raft_pstate_t) root = POBJ_ROOT(pop_raft_state, raft_pstate_t);
    TOID(struct circular_log) log = D_RO(root)->log;
    raft_vote(raft_handle, D_RO(root)->voted_for);
    raft_set_current_term(raft_handle, D_RO(root)->term);
    unsigned long ptr = D_RO(log)->log_head;
    raft_entry_t ety;
    while(ptr != D_RO(log)->log_tail) {
      ptr = read_from_log((unsigned char *)&ety, ptr);
      ety.data.buf = malloc(ety.data.len);
      ptr = read_from_log((unsigned char *)ety.data.buf, ptr);
      raft_append_entry(raft_handle, &ety);
    }
  }
  raft_set_election_timeout(raft_handle, PERIODICITY_MSEC);
  /* Launch cyclone service */
  threadpool.create_thread(boost::bind(&boost::asio::io_service::run,
				       &ioService));
  monitor_obj    = new monitor_incoming();
  monitor_thread = new boost::thread(boost::ref(*monitor_obj));
}

void cyclone_shutdown()
{
  /* Shutdown */
  ioService.stop();
  threadpool.join_all();
  monitor_obj->terminate = true;
  monitor_thread->join();
  for(int i=0;i<=replicas;i++) {
    zmq_close(zmq_push_sockets[i]);
    zmq_close(zmq_pull_sockets[i]);
  }
  delete[] zmq_push_sockets;
  delete[] zmq_pull_sockets;
  pmemobj_close(pop_raft_state);
}
