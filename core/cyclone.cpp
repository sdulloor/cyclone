// Asynchronous fault tolerant pmem log replication with cyclone
#include "cyclone.hpp"
#include "cyclone_context.hpp"
#include "tuning.hpp"
#include "checkpoint.hpp"
#include <rte_ring.h>
#include <rte_mbuf.h>

extern dpdk_context_t * global_dpdk_context;
struct rte_ring ** to_cores;
struct rte_ring *from_cores;

/** Raft callback for sending request vote message */
static int __send_requestvote(raft_server_t* raft,
			      void *user_data,
			      raft_node_t *node,
			      msg_requestvote_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)user_data;
  void *socket      = raft_node_get_udata(node);
  msg_t msg;
  msg.source      = cyclone_handle->me;
  msg.msg_type    = MSG_REQUESTVOTE;
  msg.rv          = *m;
  rte_mbuf *mb = rte_pktmbuf_alloc(global_dpdk_context->mempools[q_raft]);
  if(mb == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for send requestvote";
    exit(-1);
  }
  cyclone_prep_mbuf(global_dpdk_context,
		    (int)(unsigned long)socket,
		    q_raft,
		    mb,
		    &msg,
		    sizeof(msg_t));
  cyclone_tx(global_dpdk_context, mb, q_raft);
  return 0;
}

int cyclone_serialize_last_applied(void *cyclone_handle, void *buf)
{
  char *buffer = (char *)buf;
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  int size = 0;
  raft_entry_t *ety = raft_last_applied_ety(handle->raft_handle);
  if(ety != NULL) {
    memcpy(buffer, ety, sizeof(raft_entry_t));
    size += sizeof(raft_entry_t);
    buffer  += sizeof(raft_entry_t);
    /* TBD
    (void)handle->read_from_log((unsigned char *)buffer, 
				(unsigned long)ety->data.buf);
    */
    buffer  += ety->data.len;
    size    += ety->data.len;
  }
  return size;
}

void cyclone_deserialize_last_applied(void *cyclone_handle, raft_entry_t *ety)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  /* TBD
  ety->data.buf = (void *)
    handle->double_append_to_raft_log
    ((unsigned char *)ety,
     sizeof(raft_entry_t),
     (unsigned char *)(ety + 1), 
     ety->data.len);
  */
}


/** Raft callback for sending appendentries message */
static void __send_appendentries_response(void *udata,
					  raft_node_t *node,
					  msg_appendentries_response_t* r)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  msg_t resp;
  void *socket  = raft_node_get_udata(node);
  resp.msg_type = MSG_APPENDENTRIES_RESPONSE;
  memcpy(&resp.aer, r, sizeof(msg_appendentries_response_t));
  resp.source = cyclone_handle->me;
  rte_mbuf *m = rte_pktmbuf_alloc(global_dpdk_context->mempools[q_raft]);
  if(m == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for send requestvote";
    exit(-1);
  }
  cyclone_prep_mbuf(global_dpdk_context,
		    (int)(unsigned long)socket,
		    q_raft,
		    m,
		    &resp,
		    sizeof(msg_t));
  cyclone_tx(global_dpdk_context, m, q_raft);
}

/** Raft callback for sending appendentries message */
static int __send_appendentries(raft_server_t* raft,
				void *udata,
				raft_node_t *node,
				msg_appendentries_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  void *socket      = raft_node_get_udata(node);
  msg_t *msg = (msg_t *)cyclone_handle->cyclone_buffer_out;
  unsigned char *ptr = (unsigned char *)(msg + 1);
  msg->msg_type         = MSG_APPENDENTRIES;
  msg->source           = cyclone_handle->me;
  msg->ae.term          = m->term;
  msg->ae.prev_log_idx  = m->prev_log_idx;
  msg->ae.prev_log_term = m->prev_log_term;
  msg->ae.leader_commit = m->leader_commit;
  msg->ae.n_entries     = 0;
  rte_mbuf *mb = rte_pktmbuf_alloc(global_dpdk_context->mempools[q_raft]);
  if(mb == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for send requestvote";
  }
  cyclone_prep_mbuf(global_dpdk_context,
		    (int)(unsigned long)socket,
		    q_raft,
		    mb,
		    msg,
		    ptr - cyclone_handle->cyclone_buffer_out);
  cyclone_tx(global_dpdk_context, mb, q_raft);
  return m->n_entries;
}


/** Raft callback for sending appendentries message */
static int __send_appendentries_opt(raft_server_t* raft,
				    void *udata,
				    raft_node_t *node,
				    msg_appendentries_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  void *socket      = (void *)raft_node_get_udata(node);
  if(m->n_entries == 0)
    return __send_appendentries(raft, udata, node, m);
  int pkts_out = (PKT_BURST > m->n_entries) ? m->n_entries:PKT_BURST;
  //int pkts_out = 1;
  int i;
  int tx = 0;
  for(i=0;i<pkts_out;i++) {
    rte_mbuf *b = (rte_mbuf *)m->entries[i].pkt;
    msg_t *msg = pktadj2msg(b);
    msg->ae.leader_commit = m->leader_commit;
    msg->ae.term          = m->term;
    msg->source        = cyclone_handle->me;
    // Bump refcnt, add ethernet header and handoff for transmission
    rte_mbuf *e = rte_pktmbuf_alloc(global_dpdk_context->extra_pool);
    if(e == NULL) {
      BOOST_LOG_TRIVIAL(info) << "Unable to allocate header ";
      break;
    }
    
    rte_mbuf *bc = b;
    rte_pktmbuf_refcnt_update(bc, 1);
    
    /* prepend new header */
    e->next = bc;
    /* update header's fields */
    e->pkt_len = (uint16_t)(e->data_len + bc->pkt_len);
    e->nb_segs = (uint8_t)(bc->nb_segs + 1);
    /* copy metadata from source packet*/
    e->port = bc->port;
    e->vlan_tci = bc->vlan_tci;
    e->vlan_tci_outer = bc->vlan_tci_outer;
    e->tx_offload = bc->tx_offload;
    e->hash = bc->hash;
    e->ol_flags = bc->ol_flags;

    struct ether_hdr *eth = (struct ether_hdr *)rte_pktmbuf_prepend(e, sizeof(struct ether_hdr));
    cyclone_prep_eth(global_dpdk_context, (int)(unsigned long)socket, q_raft, eth);
    //rte_mbuf_sanity_check(e, 1);
    tx += cyclone_buffer_pkt(global_dpdk_context, e, q_raft);
  }
  tx += cyclone_flush_buffer(global_dpdk_context, q_raft);
  return tx;
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
		      raft_entry_t *ety,
		      int ety_idx)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  unsigned char *chunk = (unsigned char *)pktadj2rpc((rte_mbuf *)ety->pkt);
  int delta_node_id;
  if(ety->type != RAFT_LOGTYPE_ADD_NODE) {    
    int seg_no = 0;
    char *pkt_end;
    rte_mbuf *m = (rte_mbuf *)(ety->pkt);
    while(m != NULL) {
      rpc_t *rpc;
      if(seg_no == 0) {
	rpc = pktadj2rpc(m);
      }
      else {
	rpc = rte_pktmbuf_mtod(m, rpc_t *);
      }
      pkt_end = rte_pktmbuf_mtod_offset(m, char *, m->data_len);
      while(true) {
	rpc->wal.rep = REP_SUCCESS;
	__sync_synchronize();
	char *point = (char *)rpc;
	point = point + sizeof(rpc_t);
	point = point + rpc->payload_sz;
	if(point >= pkt_end)
	  break;
	rpc = (rpc_t *)point;
      }
      m = m->next;
      seg_no++;
    }
  }

  if(ety->type == RAFT_LOGTYPE_REMOVE_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)(chunk + sizeof(rpc_t));
    delta_node_id = cfg->node;
    BOOST_LOG_TRIVIAL(info) << "SHUTDOWN node " << delta_node_id;
    if(delta_node_id == cyclone_handle->me) {
      exit(-1);
    }
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NONVOTING_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)(chunk + sizeof(rpc_t));
    delta_node_id = cfg->node;
    BOOST_LOG_TRIVIAL(info) << "INIT nonvoting node " << delta_node_id;
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NODE) {
    int *delta_node_idp = (int *)chunk;
    BOOST_LOG_TRIVIAL(info) << "STARTUP node " << *delta_node_idp;
  }
  return 0;
}

static void handle_cfg_change(cyclone_t * cyclone_handle,
			      raft_entry_t *ety,
			      void *chunk)
{
  if(ety->type == RAFT_LOGTYPE_ADD_NONVOTING_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)((char *)chunk + sizeof(rpc_t));
    int delta_node_id = cfg->node;
    // call raft add non-voting node
    raft_add_non_voting_node(cyclone_handle->raft_handle,
			     cfg->last_included_idx,
			     //cyclone_handle->router->output_socket(delta_node_id),
			     NULL,
			     delta_node_id,
			     delta_node_id == cyclone_handle->me ? 1:0);
    
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NODE) {
    int delta_node_id = *(int *)chunk;
    // call raft add node
    raft_add_node(cyclone_handle->raft_handle,
		  //cyclone_handle->router->output_socket(delta_node_id),
		  NULL,
		  delta_node_id,
		  delta_node_id == cyclone_handle->me ? 1:0);
  }
  else if(ety->type == RAFT_LOGTYPE_REMOVE_NODE) {
    cfg_change_t *cfg = (cfg_change_t *)((char *)chunk + sizeof(rpc_t));
    int delta_node_id = cfg->node;
    // call raft remove node
    raft_remove_node(cyclone_handle->raft_handle,
		     raft_get_node(cyclone_handle->raft_handle,
				   delta_node_id));
  }
}

static void add_head(void *pkt,
		     cyclone_t *cyclone_handle,
		     raft_entry_t *ety,
		     raft_entry_t *ety_prev,
		     int ety_index)
{
  rte_mbuf *m = (rte_mbuf *)pkt;
  struct ipv4_hdr *ip = rte_pktmbuf_mtod(m, struct ipv4_hdr *);
  initialize_ipv4_header(m,
			 ip,
			 magic_src_ip, 
			 q_raft,
			 m->pkt_len - sizeof(struct ipv4_hdr));
  msg_t *hdr = pktadj2msg(m);
  hdr->msg_type         = MSG_APPENDENTRIES;
  //ae.term to be filled in at tx time
  //ae.Leader commit to be filled at tx time
  hdr->ae.prev_log_idx  = ety_index;
  hdr->ae.n_entries     = 1;
  if(ety_prev != NULL) {
    hdr->ae.prev_log_term = ety_prev->term;
  }
  else {
    hdr->ae.prev_log_term = 0;
  }
  msg_entry_t *entry = (msg_entry_t *)(hdr + 1);
  memcpy(entry, ety, sizeof(raft_entry_t));
}

/** Raft callback for appending an item to the log */
static int __raft_logentry_offer_batch(raft_server_t* raft,
      				       void *udata,
      				       raft_entry_t *ety,
				       raft_entry_t *prev,
      				       int ety_idx,
      				       int count)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  struct circular_log *log = cyclone_handle->log;
  unsigned long tail = log->tail;
  raft_entry_t *e = ety;
  for(int i=0; i<count;i++,e++) {
    if(cyclone_is_leader(cyclone_handle)) {
      add_head(e->data.buf, cyclone_handle, e, prev, ety_idx + i);
    }
    e->id = ety_idx + i;
    rte_mbuf *m = (rte_mbuf *)e->data.buf;
    rte_mbuf *saved_head = m;
    // Stash away a reference. 
    e->pkt = m;
    if(e->pkt == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to create clone";
      exit(-1);
    }
    prev = e;
    e->data.buf = (void *)tail;
    int seg_no = 0;
    char *pkt_end;
    while(m != NULL) {
      rpc_t *rpc;
      if(seg_no == 0) {
	rpc = pktadj2rpc(m);
      }
      else {
	rpc = rte_pktmbuf_mtod(m, rpc_t *);
      }
      pkt_end = rte_pktmbuf_mtod_offset(m, char *, m->data_len);
      while(true) {
	//handle_cfg_change(cyclone_handle, e, (unsigned char *)rpc);
	if(e->type != RAFT_LOGTYPE_ADD_NODE) { 
	  rpc->wal.rep = REP_UNKNOWN;
	  int core = rpc->client_id % executor_threads;
	  //Increment refcount handoff segment for exec 
	  rte_mbuf_refcnt_update(m, 1);
	  void *pair[2];
	  pair[0] = m;
	  pair[1] = rpc;
	  if(rte_ring_sp_enqueue_bulk(to_cores[core], pair, 2) == -ENOBUFS) {
	    BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
	    exit(-1);
	  }
	}
	char *point = (char *)rpc;
	point = point + sizeof(rpc_t);
	point = point + rpc->payload_sz;
	if(point >= pkt_end)
	  break;
	rpc = (rpc_t *)point;
      }
      m = m->next;
      seg_no++;
    }
    // Flush the packet to NVM
    persist_mbuf(saved_head);
    // Add to log
    tail = log_offer(log, saved_head, tail, cyclone_handle->RAFT_LOGENTRIES);
    if(tail == -1) {
      BOOST_LOG_TRIVIAL(fatal) << "Out of raft logspace !";
      exit(-1);
    }

  }
  log_persist(log, tail, cyclone_handle->RAFT_LOGENTRIES);
  return 0;
}

static void free_mbuf_chain(rte_mbuf *m)
{
  rte_mbuf * tmp;
  while(m != NULL) {
    tmp = m;
    m = m->next;
    __sync_synchronize(); // Must get next before decrementing refcnt
    rte_pktmbuf_free_seg(tmp);
  }
}

static int __raft_logentry_poll_batch(raft_server_t* raft,
				      void *udata,
				      raft_entry_t *entry,
				      int ety_idx,
				      int cnt)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  log_poll_batch(cyclone_handle->log, cnt, cyclone_handle->RAFT_LOGENTRIES);
  for(int i=0;i<cnt;i++) {
    free_mbuf_chain((rte_mbuf *)entry->pkt);
    entry++;
  }
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
  log_pop(cyclone_handle->log, cyclone_handle->RAFT_LOGENTRIES);
  if(entry->type != RAFT_LOGTYPE_ADD_NODE) {
    int seg_no = 0;
    char *pkt_end;
    rte_mbuf *m = (rte_mbuf *)(entry->pkt);
    while(m != NULL) {
      rpc_t *rpc;
      if(seg_no == 0) {
	rpc = pktadj2rpc(m);
      }
      else {
	rpc = rte_pktmbuf_mtod(m, rpc_t *);
      }
      pkt_end = rte_pktmbuf_mtod_offset(m, char *, m->data_len);
      while(true) {
	__sync_synchronize();
	rpc->wal.rep = REP_FAILED;
	__sync_synchronize();
	char *point = (char *)rpc;
	point = point + sizeof(rpc_t);
	point = point + rpc->payload_sz;
	if(point >= pkt_end)
	  break;
	rpc = (rpc_t *)point;
      }
      m = m->next;
      seg_no++;
    }
    free_mbuf_chain((rte_mbuf *)entry->pkt);
  }
  return result;
}

/** Raft callback for detecting when a node has sufficient logs */
void __raft_has_sufficient_logs(raft_server_t *raft,
				void *user_data,
				raft_node_t *node)
{
  msg_entry_t client_req;
  msg_entry_response_t *client_rep;
  cyclone_t* cyclone_handle = (cyclone_t *)user_data;
  client_req.id = rand();
  if(client_req.id == 0) {
    client_req.id = 1;
  }
  client_req.data.buf = malloc(sizeof(int));
  *(int *)client_req.data.buf = raft_node_get_id(node);
  BOOST_LOG_TRIVIAL(info) << "NODE HAS SUFFICIENT LOGS " << *(int *)client_req.data.buf;
  client_req.data.len = sizeof(int);
  client_req.type = RAFT_LOGTYPE_ADD_NODE;
  // TBD: Handle error
  client_rep = (msg_entry_response_t *)malloc(sizeof(msg_entry_response_t));
  (void)raft_recv_entry(cyclone_handle->raft_handle, 
			&client_req, 
			client_rep);
  free(client_rep);
}

/** Raft callback for displaying debugging information */
void __raft_log(raft_server_t* raft, 
		raft_node_t *node,
		void *udata, 
		const char *buf)
{
  //BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT " << buf;
}

/** Raft callback for displaying debugging information (elections)*/
void __raft_log_election(raft_server_t* raft, 
			 raft_node_t *node,
			 void *udata, 
			 const char *buf)
{
  //BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT::ELECTION " << buf;
}


raft_cbs_t raft_funcs = {
  __send_requestvote,
  __send_appendentries_opt,
  __send_appendentries_response,
  __applylog,
  __persist_vote,
  __persist_term,
  NULL,
  __raft_logentry_offer_batch,
  NULL,
  __raft_logentry_poll_batch,
  __raft_logentry_pop,
  __raft_has_sufficient_logs,
  NULL,//__raft_log,
  NULL//__raft_log_election
};

int cyclone_is_leader(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  if(handle->replicas == 1)
    return 1;
  return raft_is_leader(handle->raft_handle);
}

int cyclone_get_leader(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  return raft_get_current_leader(handle->raft_handle);
}

int cyclone_get_term(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  return raft_get_current_term(handle->raft_handle);
}

int dpdk_raft_monitor(void *arg)
{
  struct cyclone_monitor *monitor = (struct cyclone_monitor *)arg;
  (*monitor)();
  return 0;
}

static struct cyclone_img_load_st {
  cyclone_t * cyclone_handle;
  cyclone_build_image_t cyclone_build_image_callback;
  void operator ()()
  {
    __sync_synchronize(); // Taking over socket
    //cyclone_build_image_callback(cyclone_handle->router->control_input_socket());
    cyclone_build_image_callback(NULL);
    raft_unset_img_build(cyclone_handle->raft_handle);
  }
  
} cyclone_image_loader;

void* cyclone_boot(const char *config_quorum_path,
		   void *router,
		   cyclone_build_image_t cyclone_build_image_callback,
		   int me,
		   int clients,
		   void *user_arg)
{
  cyclone_t *cyclone_handle;
  std::stringstream key;
  std::stringstream addr;
  char ringname[50];

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
				   RING_F_SC_DEQ|RING_F_SP_ENQ); 
  }
  

  cyclone_handle = new cyclone_t();
  cyclone_handle->user_arg   = user_arg;
  
  boost::property_tree::read_ini(config_quorum_path, cyclone_handle->pt);
  std::string path_raft           = cyclone_handle->pt.get<std::string>("storage.raftpath");
  char me_str[100];
  sprintf(me_str, "%d", me);
  path_raft.append(me_str);
  cyclone_handle->RAFT_LOGENTRIES = cyclone_handle->pt.get<int>("storage.logsize")/8;
  cyclone_handle->replicas        = cyclone_handle->pt.get<int>("quorum.replicas");
  cyclone_handle->me              = me;
  int baseport  = cyclone_handle->pt.get<int>("quorum.baseport"); 
  cyclone_handle->raft_handle = raft_new();
  raft_set_multi_inflight(cyclone_handle->raft_handle);

  /* Setup raft state */
  if(access(path_raft.c_str(), F_OK)) {
    // TBD: figure out how to make this atomic
    cyclone_handle->pop_raft_state = pmemobj_create(path_raft.c_str(),
						    POBJ_LAYOUT_NAME(raft_persistent_state),
						    8*cyclone_handle->RAFT_LOGENTRIES + PMEMOBJ_MIN_POOL,
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
		 (sizeof(struct circular_log) + 8*cyclone_handle->RAFT_LOGENTRIES));
      log_t log = D_RO(root)->log;
      cyclone_handle->log = D_RW(log);
      TX_ADD(log);
      D_RW(log)->head = 0;
      D_RW(log)->tail = 0;
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
    cyclone_handle->log = D_RW(log);
    /* TBD
    raft_vote(cyclone_handle->raft_handle, 
	      raft_get_node(cyclone_handle->raft_handle,
			    D_RO(root)->voted_for));
    raft_set_current_term(cyclone_handle->raft_handle, 
			  D_RO(root)->term);
    unsigned long ptr = D_RO(log)->log_head;
    raft_entry_t ety;
    while(ptr != D_RO(log)->log_tail) {
      // Optimize later by removing transaction
      ptr = cyclone_handle->read_from_log((unsigned char *)&ety, ptr);
      ety.data.buf = (void *)ptr;
      ptr = cyclone_handle->skip_log_entry(ptr);
      raft_append_entry(cyclone_handle->raft_handle, &ety);
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovery complete";
    */
  }
  // Note: set raft callbacks AFTER recovery
  raft_set_callbacks(cyclone_handle->raft_handle, &raft_funcs, cyclone_handle);
  raft_set_election_timeout(cyclone_handle->raft_handle, RAFT_ELECTION_TIMEOUT);
  raft_set_request_timeout(cyclone_handle->raft_handle, RAFT_REQUEST_TIMEOUT);
  raft_set_nack_timeout(cyclone_handle->raft_handle, RAFT_NACK_TIMEOUT);
  raft_set_log_target(cyclone_handle->raft_handle, RAFT_LOG_TARGET);

  /* setup connections */
  cyclone_handle->router = (quorum_switch *)router;
  bool i_am_active = false;
  for(int i=0;i<cyclone_handle->pt.get<int>("active.replicas");i++) {
    char nodeidxkey[100];
    sprintf(nodeidxkey, "active.entry%d",i);
    int nodeidx = cyclone_handle->pt.get<int>(nodeidxkey);
    if(nodeidx == cyclone_handle->me) {
      i_am_active = true;
    }
    raft_add_peer(cyclone_handle->raft_handle,
		  (void *)(unsigned long)cyclone_handle->router->replica_mc(nodeidx),
		  nodeidx,
		  nodeidx == cyclone_handle->me ? 1:0);
  }

  cyclone_handle->cyclone_buffer_in  = new unsigned char[MSG_MAXSIZE];
  cyclone_handle->cyclone_buffer_out = new unsigned char[MSG_MAXSIZE];
  cyclone_handle->monitor_obj    = new cyclone_monitor();
  cyclone_handle->monitor_obj->cyclone_handle    = cyclone_handle;

  // Must activate myself
  if(!i_am_active) {
    raft_set_img_build(cyclone_handle->raft_handle);
    raft_add_peer(cyclone_handle->raft_handle,
		  (void *)(unsigned long)cyclone_handle->router->replica_mc(cyclone_handle->me),
		  cyclone_handle->me,
		  1);

    // Obtain and load checkpoint
    int loaded_term, loaded_idx, master;
    raft_entry_t *init_ety;
    init_build_image(//cyclone_handle->router->control_input_socket(),
		     NULL,
		     &loaded_term,
		     &loaded_idx,
		     &master,
		     (void **)&init_ety);
    if(init_ety != NULL) {
      cyclone_deserialize_last_applied(cyclone_handle, init_ety);
    }
    raft_loaded_checkpoint(cyclone_handle->raft_handle,
			   loaded_term, 
			   loaded_idx,
			   init_ety,
			   master);
    cyclone_image_loader.cyclone_handle = cyclone_handle;
    cyclone_image_loader.cyclone_build_image_callback = cyclone_build_image_callback;
    cyclone_handle->checkpoint_thread = new boost::thread(boost::ref(cyclone_image_loader));
  }
  /* Launch cyclone service */
  __sync_synchronize(); // Going to give the thread control over the socket
  int e = rte_eal_remote_launch(dpdk_raft_monitor, (void *)cyclone_handle->monitor_obj, 1);
  if(e != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to launch raft monitor on remote lcore";
    exit(-1);
  }
  return cyclone_handle;
}

void cyclone_shutdown(void *cyclone_handle)
{
  // TBD
}
