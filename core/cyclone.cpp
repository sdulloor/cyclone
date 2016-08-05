// Asynchronous fault tolerant pmem log replication with cyclone
#include "cyclone.hpp"
#include "cyclone_context.hpp"
#include "tuning.hpp"
#include "checkpoint.hpp"
#include <rte_ring.h>
#include <rte_mbuf.h>

#if defined(DPDK_STACK)
extern void * global_dpdk_context;
#endif

struct rte_ring ** to_cores;
struct rte_ring *from_cores;

void *cyclone_control_socket_out(void *cyclone_handle, 
				 int replica)
{
  return ((cyclone_t *)cyclone_handle)->router->control_output_socket(replica);
}
void *cyclone_control_socket_in(void *cyclone_handle)
{
  return ((cyclone_t *)cyclone_handle)->router->control_input_socket();
}

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
  cyclone_tx(socket, 
	      (unsigned char *)&msg, 
	      sizeof(msg_t), 
	      "__send_requestvote");
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
    (void)handle->read_from_log((unsigned char *)buffer, 
				(unsigned long)ety->data.buf);
    buffer  += ety->data.len;
    size    += ety->data.len;
  }
  return size;
}

void cyclone_deserialize_last_applied(void *cyclone_handle, raft_entry_t *ety)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  ety->data.buf = (void *)
    handle->double_append_to_raft_log
    ((unsigned char *)ety,
     sizeof(raft_entry_t),
     (unsigned char *)(ety + 1), 
     ety->data.len);
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
  cyclone_tx(socket,
	     (unsigned char *)&resp, 
	     sizeof(msg_t), 
	     "APPENDENTRIES RESP async");
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
  unsigned long spc_remains = MSG_MAXSIZE - sizeof(msg_t);
  int i;
  for(i=0;i<m->n_entries;i++) {
    unsigned long spc_needed = sizeof(msg_entry_t) + m->entries[i].data.len;
    if(spc_needed > spc_remains) {
      break;
    }
    spc_remains -= spc_needed;
    memcpy(ptr, &m->entries[i], sizeof(msg_entry_t));
    ptr += sizeof(msg_entry_t);
  }
  // Adjust number of entries
  m->n_entries          = i;
  msg->ae.n_entries     = m->n_entries;
  for(i=0;i<m->n_entries;i++) {
    (void)cyclone_handle->read_from_log_check_size(ptr,
						   (unsigned long)m->entries[i].data.buf,
						   m->entries[i].data.len);

    ptr += m->entries[i].data.len;
  }
  cyclone_tx(socket, 
	      cyclone_handle->cyclone_buffer_out, 
	      ptr - cyclone_handle->cyclone_buffer_out, 
	      "__send_requestvote");
  return m->n_entries;
}


/** Raft callback for sending appendentries message */
static int __send_appendentries_opt(raft_server_t* raft,
				    void *udata,
				    raft_node_t *node,
				    msg_appendentries_t* m)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  dpdk_socket_t *socket      = (dpdk_socket_t *)raft_node_get_udata(node);
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
    /*
    msg_entry_t *mentry = (msg_entry_t *)(msg + 1);
    cyclone_tx_eth(socket, 
		   rte_pktmbuf_mtod(b, unsigned char *),
		   b->data_len,
		   "__send_requestvote");
    continue;
    */
    // Bump refcnt, add ethernet header and handoff for transmission
    rte_mbuf *e = rte_pktmbuf_alloc(socket->extra_pool);
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
    memset(eth, 0, sizeof(struct ether_hdr));
    ether_addr_copy(&socket->remote_mac, &eth->d_addr);
    ether_addr_copy(&socket->local_mac, &eth->s_addr);
    eth->ether_type = rte_cpu_to_be_16(ETHER_TYPE_IPv4);

    
    /*
    if(rte_pktmbuf_chain(e, bc)) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to chain";
      exit(-1);
    }
    */
    //rte_mbuf_sanity_check(e, 1);
    tx += cyclone_buffer_pkt(socket, e);
  }
  tx += cyclone_flush_socket(socket);
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

static void __client_assist_ok(void *udata,
			       replicant_t *rep)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  rpc_t rpc;
  rpc.code = RPC_REP_ASSIST_OK;
  rpc.requestor = cyclone_handle->me;
  rpc.rep = *rep;
  rpc.channel_seq = rep->channel_seq;
  cyclone_tx(cyclone_handle->router->cpaths.socket(rep->client_mc, rep->client_id),
	     (const unsigned char *)&rpc,
	     sizeof(rpc_t),
	     "Client assist response from replica");  
}


static int __applylog(raft_server_t* raft,
		      void *udata,
		      raft_entry_t *ety,
		      int ety_idx)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  unsigned char *chunk = (unsigned char *)pktadj2rpc((rte_mbuf *)ety->pkt);
  int delta_node_id;
  if(ety->type != RAFT_LOGTYPE_ADD_NODE && cyclone_handle->cyclone_commit_cb != NULL) {    

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
			     cyclone_handle->router->output_socket(delta_node_id),
			     delta_node_id,
			     delta_node_id == cyclone_handle->me ? 1:0);
    
  }
  else if(ety->type == RAFT_LOGTYPE_ADD_NODE) {
    int delta_node_id = *(int *)chunk;
    // call raft add node
    raft_add_node(cyclone_handle->raft_handle,
		  cyclone_handle->router->output_socket(delta_node_id),
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
static int __raft_logentry_offer(raft_server_t* raft,
				 void *udata,
				 raft_entry_t *ety,
				 raft_entry_t *prev,
				 int ety_idx,
				 replicant_t *rep)
{
  BOOST_LOG_TRIVIAL(fatal) << "Only batch mode is supported";
  exit(-1);
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  if(cyclone_is_leader(cyclone_handle)) {
    add_head(ety->data.buf, cyclone_handle, ety, prev, ety_idx);
  }
  unsigned char *chunk    = (unsigned char *)pktadj2rpc((rte_mbuf *)ety->data.buf);
  if(rep != NULL) {
    rep->server_id = cyclone_handle->me;
  }
  ety->id = ety_idx;
  ety->data.buf = (void *)
    cyclone_handle->double_append_to_raft_log
    ((unsigned char *)ety,
     sizeof(raft_entry_t),
     chunk,
     ety->data.len);
  handle_cfg_change(cyclone_handle, ety, chunk);
  if(rep != NULL) {
    memcpy(&rep->ety, ety, sizeof(raft_entry_t));
  }
  if(cyclone_handle->cyclone_rep_cb != NULL && ety->type != RAFT_LOGTYPE_ADD_NODE) {    
    rpc_t *rpc = (rpc_t *)chunk;
    rpc->wal.rep = REP_UNKNOWN;
    rpc->wal.raft_term = ety->term;
    rpc->wal.raft_idx  = ety_idx;
    ety->pkt = ety->data.buf; // Stash away the pkt
    rte_mbuf *m = (rte_mbuf *)ety->pkt;
    // Handoff for execution
    int core = rpc->client_id % executor_threads;
    if(rte_ring_sp_enqueue(to_cores[core], m) == -ENOBUFS) {
      BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
      exit(-1);
    }
    /*
      cyclone_handle->cyclone_rep_cb(cyclone_handle->user_arg,
      (const unsigned char *)chunk,
      ety->data.len,
      ety_idx,
      ety->term,
      rep);
    */
  }
  return result;
}


/** Raft callback for appending an item to the log */
static int __raft_logentry_offer_batch(raft_server_t* raft,
      				       void *udata,
      				       raft_entry_t *ety,
				       raft_entry_t *prev,
      				       int ety_idx,
      				       int count,
      				       replicant_t *rep)
{
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  TOID(raft_pstate_t) root = POBJ_ROOT(cyclone_handle->pop_raft_state, raft_pstate_t);
  log_t tmp = D_RO(root)->log;
  struct circular_log *log = D_RW(tmp);
  unsigned long tail = log->log_tail;
  raft_entry_t *e = ety;
  dpdk_socket_t *sock = (dpdk_socket_t *)cyclone_handle->router->input_socket();
  for(int i=0; i<count;i++,e++) {
    if(cyclone_is_leader(cyclone_handle)) {
      add_head(e->data.buf, cyclone_handle, e, prev, ety_idx + i);
    }
    e->id = ety_idx + i;
    rte_mbuf *m = (rte_mbuf *)e->data.buf;

    // Stash away a clone. 
    e->pkt = (void *)rte_pktmbuf_clone(m, sock->clone_pool); 
    if(e->pkt == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to create clone";
      exit(-1);
    }
    prev = e;
    tail = cyclone_handle->append_to_raft_log_noupdate(log,
      						       (unsigned char *)e,
      						       sizeof(raft_entry_t),
      						       tail);
    e->data.buf = (void *)tail;
    int seg_no = 0;
    char *pkt_end;
    if(cyclone_handle->log_space_left(log, tail) < (2*sizeof(int) + e->data.len)) {
      BOOST_LOG_TRIVIAL(fatal) << "Out of RAFT logspace !!";
      exit(-1);
    }
    copy_to_circular_log(cyclone_handle->pop_raft_state,
			 log,
			 cyclone_handle->RAFT_LOGSIZE,
			 tail, 
			 (unsigned char *)&e->data.len,
			 sizeof(int));
    tail = circular_log_advance_ptr(tail, sizeof(int), cyclone_handle->RAFT_LOGSIZE);
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
	copy_to_circular_log(cyclone_handle->pop_raft_state,
			     log,
			     cyclone_handle->RAFT_LOGSIZE,
			     tail, 
			     (unsigned char *)rpc,
			     sizeof(rpc_t) + rpc->payload_sz);
	tail = circular_log_advance_ptr(tail, 
					sizeof(rpc_t) + rpc->payload_sz, 
					cyclone_handle->RAFT_LOGSIZE);
	//handle_cfg_change(cyclone_handle, e, (unsigned char *)rpc);
	if(e->type != RAFT_LOGTYPE_ADD_NODE) { 
	  rpc->wal.rep = REP_UNKNOWN;
	  rpc->wal.raft_term = e->term;
	  rpc->wal.raft_idx  = ety_idx + i;
	  int core = rpc->client_id % executor_threads;
	  //Increment refcount handoff segment for exec 
	  rte_mbuf_refcnt_update(m, 1);
	  if(rte_ring_sp_enqueue(to_cores[core], m) == -ENOBUFS) {
	    BOOST_LOG_TRIVIAL(fatal) << "raft->core comm ring is full";
	    exit(-1);
	  }
	  if(rte_ring_sp_enqueue(to_cores[core], rpc) == -ENOBUFS) {
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
      // Release this mbuf -- we still have the clone
      rte_mbuf_refcnt_update(m, -1);
      m = m->next;
      seg_no++;
    }
    copy_to_circular_log(cyclone_handle->pop_raft_state,
			 log,
			 cyclone_handle->RAFT_LOGSIZE,
			 tail, 
			 (unsigned char *)&e->data.len,
			 sizeof(int));
    tail = circular_log_advance_ptr(tail, sizeof(int), cyclone_handle->RAFT_LOGSIZE);
    
  }

  persist_to_circular_log(cyclone_handle->pop_raft_state, 
			  log,
			  cyclone_handle->RAFT_LOGSIZE,
			  log->log_tail,
			  tail - log->log_tail);

  log->log_tail = tail;

  clflush(&log->log_tail, sizeof(unsigned long));

  return 0;
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
  if(ety_idx != -1) {
    rte_pktmbuf_free((rte_mbuf *)entry->pkt);
    cyclone_handle->double_remove_head_raft_log();
  }
  else {
    rte_pktmbuf_free((rte_mbuf *)entry->data.buf);
  }
  return result;
}

static int __raft_logentry_poll_batch(raft_server_t* raft,
				      void *udata,
				      raft_entry_t *entry,
				      int ety_idx,
				      int cnt)
{
  int result = 0;
  cyclone_t* cyclone_handle = (cyclone_t *)udata;
  for(int i=0;i<cnt;i++) {
    rte_pktmbuf_free((rte_mbuf *)entry->pkt);
    entry++;
  }
  cyclone_handle->double_remove_head_raft_log_cnt(cnt);
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
  if(cyclone_handle->cyclone_pop_cb != NULL && entry->type != RAFT_LOGTYPE_ADD_NODE) {
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
    rte_pktmbuf_free((rte_mbuf *)entry->pkt);
  }
  cyclone_handle->double_remove_tail_raft_log();
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
  BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT " << buf;
}

/** Raft callback for displaying debugging information (elections)*/
void __raft_log_election(raft_server_t* raft, 
			 raft_node_t *node,
			 void *udata, 
			 const char *buf)
{
  BOOST_LOG_TRIVIAL(debug) << "CYCLONE::RAFT::ELECTION " << buf;
}


raft_cbs_t raft_funcs = {
  __send_requestvote,
  __send_appendentries_opt,
  __client_assist_ok,
  __send_appendentries_response,
  __applylog,
  __persist_vote,
  __persist_term,
  __raft_logentry_offer,
  __raft_logentry_offer_batch,
  __raft_logentry_poll,
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

void* cyclone_add_entry(void *cyclone_handle, void *data, int size)
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ;
  msg.client.ptr  = data;
  msg.client.size = size;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
  */
  return NULL;
}

void* cyclone_add_batch(void *cyclone_handle,
			void *data,
			int *sizes,
			int batch_size)
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookies = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_BATCH;
  msg.client.ptr  = data;
  msg.client.size = batch_size;
  msg.client.batch_sizes = sizes;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
  */
  return NULL;
}

void* cyclone_add_entry_cfg(void *cyclone_handle, int type, void *data, int size)
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_CFG;
  msg.client.ptr  = data;
  msg.client.size = size;
  msg.client.type = type;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
  */
  return NULL;
}

void* cyclone_add_entry_term(void *cyclone_handle, 
			     void *data, 
			     int size,
			     int term)
{/*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_TERM;
  msg.client.ptr  = data;
  msg.client.size = size;
  msg.client.term = term;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
 */
  return NULL;
}

void* cyclone_set_img_build(void *cyclone_handle)
			    
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_SET_IMGBUILD;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
  */
  return NULL;
}

void* cyclone_unset_img_build(void *cyclone_handle)
			    
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  void *cookie = NULL;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_REQ_UNSET_IMGBUILD;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return (void *)msg.client_rep;
  */
  return NULL;
}

int cyclone_check_status(void *cyclone_handle, void *cookie)
{
  /*
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  msg_t msg;
  msg.source      = handle->me;
  msg.msg_type    = MSG_CLIENT_STATUS;
  msg.client.ptr  = cookie;
  int result;
  msg.complete = 0;
  handle->comm.add_to_runqueue(&msg);
  while(!msg.complete);
  return msg.status;
  */
  return NULL;
}

static void init_log(PMEMobjpool *pop, void *ptr, void *arg)
{
  struct circular_log *log = (struct circular_log *)ptr;
  log->log_head  = 0;
  log->log_tail  = 0;
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
    cyclone_build_image_callback(cyclone_handle->router->control_input_socket());
    raft_unset_img_build(cyclone_handle->raft_handle);
  }
  
} cyclone_image_loader;

void* cyclone_boot(const char *config_path,
		   const char *config_path_client,
		   cyclone_rep_callback_t cyclone_rep_callback,
		   cyclone_callback_t cyclone_pop_callback,
		   cyclone_callback_t cyclone_commit_callback,
		   cyclone_build_image_t cyclone_build_image_callback,
		   int me,
		   int replicas,
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
  cyclone_handle->cyclone_rep_cb = cyclone_rep_callback;
  cyclone_handle->cyclone_pop_cb = cyclone_pop_callback;
  cyclone_handle->cyclone_commit_cb = cyclone_commit_callback;
  cyclone_handle->user_arg   = user_arg;
  
  boost::property_tree::read_ini(config_path, cyclone_handle->pt);
  boost::property_tree::read_ini(config_path_client, cyclone_handle->pt_client);
  std::string path_raft           = cyclone_handle->pt.get<std::string>("storage.raftpath");
  char me_str[100];
  sprintf(me_str, "%d", me);
  path_raft.append(me_str);
  cyclone_handle->RAFT_LOGSIZE    = cyclone_handle->pt.get<unsigned long>("storage.logsize");
  cyclone_handle->replicas        = replicas;
  cyclone_handle->me              = me;
  int baseport  = cyclone_handle->pt.get<int>("quorum.baseport"); 
  cyclone_handle->raft_handle = raft_new();
  
  /* Set client assist */
  const char *client_assist_env = getenv("CLIENT_ASSIST");
  if(client_assist_env != NULL) {
    BOOST_LOG_TRIVIAL(info) << "CLIENT ASSIST ON";
    raft_set_client_assist(cyclone_handle->raft_handle);
  }

#if defined(DPDK_STACK)
  raft_set_multi_inflight(cyclone_handle->raft_handle);
#endif

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
      raft_append_entry(cyclone_handle->raft_handle, &ety, NULL);
      if(cyclone_rep_callback != NULL) {
	unsigned char *chunk = (unsigned char *)malloc(ety.data.len);
	(void)cyclone_handle->read_from_log(chunk, 
					    (unsigned long)ety.data.buf);
	cyclone_rep_callback(user_arg,
			     (const unsigned char *)chunk,
			     ety.data.len,
			     ety.id,
			     ety.term,
			     NULL);
	free(chunk);
      }
    }
    BOOST_LOG_TRIVIAL(info) << "CYCLONE: Recovery complete";
  }

  // Note: set raft callbacks AFTER recovery
  raft_set_callbacks(cyclone_handle->raft_handle, &raft_funcs, cyclone_handle);
  raft_set_election_timeout(cyclone_handle->raft_handle, RAFT_ELECTION_TIMEOUT);
  raft_set_request_timeout(cyclone_handle->raft_handle, RAFT_REQUEST_TIMEOUT);
  raft_set_nack_timeout(cyclone_handle->raft_handle, RAFT_NACK_TIMEOUT);
  raft_set_log_target(cyclone_handle->raft_handle, RAFT_LOG_TARGET);

  /* setup connections */
#if defined(DPDK_STACK)
#else
  cyclone_handle->zmq_context  = zmq_init(zmq_threads); 
#endif
  cyclone_handle->router = new raft_switch(
#if defined(DPDK_STACK)
					   global_dpdk_context,
#else
					   cyclone_handle->zmq_context,
#endif
					   &cyclone_handle->pt,
					   cyclone_handle->me,
					   cyclone_handle->replicas,
					   clients,
					   &cyclone_handle->pt_client);

  bool i_am_active = false;
  for(int i=0;i<cyclone_handle->pt.get<int>("active.replicas");i++) {
    char nodeidxkey[100];
    sprintf(nodeidxkey, "active.entry%d",i);
    int nodeidx = cyclone_handle->pt.get<int>(nodeidxkey);
    if(nodeidx == cyclone_handle->me) {
      i_am_active = true;
    }
    raft_add_peer(cyclone_handle->raft_handle,
		  cyclone_handle->router->output_socket(nodeidx),
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
		  cyclone_handle->router->output_socket(cyclone_handle->me),
		  cyclone_handle->me,
		  1);

    // Obtain and load checkpoint
    int loaded_term, loaded_idx, master;
    raft_entry_t *init_ety;
    init_build_image(cyclone_handle->router->control_input_socket(),
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
#if defined(DPDK_STACK)
  int e = rte_eal_remote_launch(dpdk_raft_monitor, (void *)cyclone_handle->monitor_obj, 1);
  if(e != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to launch raft monitor on remote lcore";
    exit(-1);
  }
#else
  cyclone_handle->monitor_thread = new boost::thread(boost::ref(*cyclone_handle->monitor_obj));
#endif
  return cyclone_handle;
}

void cyclone_shutdown(void *cyclone_handle)
{
  cyclone_t* handle = (cyclone_t *)cyclone_handle;
  handle->monitor_obj->terminate = true;
  handle->monitor_thread->join();
  handle->checkpoint_thread->join();
  delete handle->monitor_obj;
  delete handle->router;
  zmq_ctx_destroy(handle->zmq_context);
  pmemobj_close(handle->pop_raft_state);
  delete[] handle->cyclone_buffer_in;
  delete[] handle->cyclone_buffer_out;
  delete handle;
}
