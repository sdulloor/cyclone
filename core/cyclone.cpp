// Asynchronous fault tolerant pmem log replication with cyclone
#include "libcyclone.hpp"
#include "cyclone_context.hpp"


#ifdef TRACING
extern void trace_pre_append(void *data, const int size);
extern void trace_post_append(void *data, const int size);
extern void trace_send_entry(void *data, const int size);
#endif

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
  cyclone_tx(socket, 
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
    (void)cyclone_handle->read_from_log(ptr,
					(unsigned long)m->entries[i].data.buf);
#ifdef TRACING
    trace_send_entry(ptr, m->entries[i].data.len);
#endif
    ptr += m->entries[i].data.len;
  }
  cyclone_tx(socket, 
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
#ifdef TRACING
  rtc_clock timer;
  timer.start();
#endif  
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->voted_for = voted_for;
  }TX_ONABORT {
    status = -1;
  } TX_END
#ifdef TRACING
  timer.stop();
  BOOST_LOG_TRIVIAL(info) << "VOTE_PERSIST_DELTA ms:" << timer.elapsed_time()/1000;
#endif
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
#ifdef TRACING
  rtc_clock timer;
  timer.start();
#endif
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    TX_ADD(root);
    D_RW(root)->term = current_term;
  } TX_ONABORT {
    status = -1;
  } TX_END
#ifdef TRACING
  timer.stop();    
  BOOST_LOG_TRIVIAL(info) << "TERM_PERSIST_DELTA ms: " << timer.elapsed_time()/1000;
#endif
  return status;
}

static int __applylog(raft_server_t* raft,
		      void *udata,
		      const unsigned char *data,
		      const int len)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  void *chunk = malloc(len);
  TX_BEGIN(cyclone_handle->pop_raft_state) {
    (void)cyclone_handle->read_from_log(chunk, (unsigned long)data);
  } TX_END
  cyclone_handle->cyclone_cb(cyclone_handle->user_arg, chunk, len);
  free(chunk);
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
#ifdef TRACING
    trace_pre_append((unsigned char *)ety->data.buf, ety->data.len);
#endif

    if(cyclone_handle->append_to_raft_log((unsigned char *)ety,
					  sizeof(raft_entry_t)) != 0) {
      pmemobj_tx_abort(-1);
    }
    void * saved_ptr = (void *)cyclone_handle->get_log_offset();
    if(cyclone_handle->append_to_raft_log((unsigned char *)ety->data.buf,
					  ety->data.len) != 0) {
      pmemobj_tx_abort(-1);
    }
    free(ety->data.buf); // release temporary memory
    ety->data.buf = saved_ptr;
#ifdef TRACING
    trace_post_append(ety->data.buf, ety->data.len);
#endif

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
  cyclone_tx(handle->zmq_push_sockets[handle->me], 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "client req");
  cyclone_rx(handle->zmq_push_sockets[handle->me],
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
  cyclone_tx(handle->zmq_push_sockets[handle->me], 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "client status");
  cyclone_rx(handle->zmq_push_sockets[handle->me],
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
  
  /* Setup raft state */
  if(access(path_raft.c_str(), F_OK)) {
    // TBD: figure out how to make this atomic
    cyclone_handle->pop_raft_state = pmemobj_create(path_raft.c_str(),
						    POBJ_LAYOUT_NAME(raft_persistent_state),
						    cyclone_handle->RAFT_LOGSIZE + PMEMOBJ_MIN_POOL,
						    0666);
    if(cyclone_handle->pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
  
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    TX_BEGIN(cyclone_handle->pop_raft_state) {
      TX_ADD(root);
      D_RW(root)->term      = 0;
      D_RW(root)->voted_for = -1;
      D_RW(root)->log = 
	TX_ALLOC(struct circular_log, 
		 (sizeof(struct circular_log) + cyclone_handle->RAFT_LOGSIZE));
      log_t log = D_RO(root)->log;
      TX_ADD(log);
      D_RW(log)->log_head = 0;
      D_RW(log)->log_tail = 0;
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to allocate log:"
	<< strerror(errno);
      exit(-1);
    } TX_END
  }
  else {
    cyclone_handle->pop_raft_state = pmemobj_open(path_raft.c_str(),
						  "raft_persistent_state");
    if(cyclone_handle->pop_raft_state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to open pmemobj pool:"
	<< strerror(errno);
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovering state";
    TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
    log_t log = D_RO(root)->log;
    raft_vote(cyclone_handle->raft_handle, D_RO(root)->voted_for);
    raft_set_current_term(cyclone_handle->raft_handle, D_RO(root)->term);
    unsigned long ptr = D_RO(log)->log_head;
    raft_entry_t ety;
    while(ptr != D_RO(log)->log_tail) {
      // Optimize later by removing transaction
      TX_BEGIN(cyclone_handle->pop_raft_state) {
	ptr = cyclone_handle->read_from_log((unsigned char *)&ety, ptr);
      } TX_END
      ety.data.buf = (void *)ptr;
      TX_BEGIN(cyclone_handle->pop_raft_state) {
	ptr = cyclone_handle->skip_log_entry(ptr);
      } TX_END
      raft_append_entry(cyclone_handle->raft_handle, &ety);
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovery complete";
  }

  // Note: set raft callbacks AFTER recovery
  raft_set_callbacks(cyclone_handle->raft_handle, &raft_funcs, cyclone_handle);
  raft_set_election_timeout(cyclone_handle->raft_handle, 1000);
  raft_set_request_timeout(cyclone_handle->raft_handle, 500);

  /* setup connections */
  cyclone_handle->zmq_context  = zmq_init(1); // One thread should be enough ?
  cyclone_handle->zmq_pull_sockets = new void*[cyclone_handle->replicas];
  cyclone_handle->zmq_push_sockets = new void*[cyclone_handle->replicas];

  // PUSH sockets
  for(int i=0;i<cyclone_handle->replicas;i++) {
    cyclone_handle->zmq_push_sockets[i] =
      cyclone_socket_out(cyclone_handle->zmq_context,
			 i == cyclone_handle->me ? 1:0);
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
    cyclone_handle->zmq_pull_sockets[i] =
      cyclone_socket_in(cyclone_handle->zmq_context,
			i == cyclone_handle->me ? 1:0);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.iface" << i;
    addr << "tcp://";
    addr << cyclone_handle->pt.get<std::string>(key.str().c_str());
    unsigned long port = baseport + cyclone_handle->me*cyclone_handle->replicas + i;
    addr << ":" << port;
    cyclone_bind_endpoint(cyclone_handle->zmq_pull_sockets[i], addr.str().c_str());
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
