#include <stdlib.h>
#include "cyclone.hpp"
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include "../core/cyclone_comm.hpp"
#include "../core/logging.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <unistd.h>
#include "cyclone_context.hpp"


extern dpdk_context_t *global_dpdk_context;
typedef struct rpc_client_st {
  int me;
  int me_mc;
  int me_queue;
  quorum_switch *router;
  rpc_t *packet_out;
  rpc_t *packet_out_aux;
  msg_t *packet_rep;
  rpc_t *packet_in;
  int server;
  int replicas;
  unsigned long channel_seq;
  dpdk_rx_buffer_t *buf;
  int server_ports;
  unsigned int *terms;

  int quorum_q(int quorum_id, int q)
  {
    return num_queues*quorum_id + q;
  }
  
  int choose_quorum(int core_mask)
  {
    return core_to_quorum(__builtin_ffs(core_mask) - 1);
  }

  int common_receive_loop(int blob_sz)
  {
    int retcode;
    int resp_sz;
    bool sent_assist_msg   = false;
    bool sent_assist_reply = false;
    unsigned long response_map = 0;
    while(true) {
      resp_sz = cyclone_rx_timeout(global_dpdk_context,
				   0,
				   me_queue,
				   buf,
				   (unsigned char *)packet_in,
				   MSG_MAXSIZE,
				   timeout_msec*1000);
      if(resp_sz == -1) {
	break;
      }

      if(packet_in->channel_seq != (channel_seq - 1)) {
	continue;
      }
      
      break;
    }
    return resp_sz;
  }

  void send_to_server(rpc_t *pkt, int sz, int quorum_id)
  {
    rte_mbuf *mb = rte_pktmbuf_alloc(global_dpdk_context->mempools[me_queue]);
    if(mb == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Out of mbufs for send to server";
    }
    pkt->wal.term = terms[quorum_id];
    cyclone_prep_mbuf_client2server(global_dpdk_context,
				    quorum_id % server_ports,
				    router->replica_mc(server),
				    quorum_q(quorum_id, q_dispatcher),
				    mb,
				    pkt,
				    sz);
    cyclone_tx(global_dpdk_context, 0, mb, me_queue);
  }


  
  int set_server()
  {
    packet_out_aux->code        = RPC_REQ_STABLE;
    packet_out_aux->flags       = 0;
    packet_out_aux->client_id   = me;
    packet_out_aux->core_mask   = 1; // Always core 0
    packet_out_aux->client_port = me_queue;
    packet_out_aux->channel_seq = channel_seq++;
    packet_out_aux->requestor   = me_mc;
    packet_out_aux->payload_sz  = 0;
    send_to_server(packet_out_aux, sizeof(rpc_t), 0); // always quorum 0
    int resp_sz = common_receive_loop(sizeof(rpc_t));
    if(resp_sz != -1) {
      memcpy(terms, packet_in + 1, num_quorums*sizeof(unsigned int));
      for(int i=0;i<num_quorums;i++) {
	terms[i] = terms[i] >> 1;
      }
      return 1;
    }
    return 0;
  }

  void update_server(const char *context)
  {
    BOOST_LOG_TRIVIAL(info) 
      << "CLIENT DETECTED POSSIBLE FAILED LEADER: "
      << server
      << " Reason " 
      << context;
    do {
      server = (server + 1)%replicas;
      BOOST_LOG_TRIVIAL(info) << "Trying " << server;
    } while(!set_server());
    BOOST_LOG_TRIVIAL(info) << "Success";
  }



  int delete_node(int core_mask, int nodeid)
  {
    int retcode;
    int resp_sz;
    int quorum_id = choose_quorum(core_mask);
    while(true) {
      packet_out->code        = RPC_REQ_NODEDEL;
      packet_out->flags       = 0;
      packet_out->client_id   = me;
      packet_out->core_mask   = core_mask;
      packet_out->client_port = me_queue;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = sizeof(cfg_change_t);
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node = nodeid;
      send_to_server(packet_out, sizeof(rpc_t) + sizeof(cfg_change_t), quorum_id);
      resp_sz = common_receive_loop(sizeof(rpc_t) + sizeof(cfg_change_t));
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_FAIL) {
	continue;
      }
      break;
    }
    return 0;
  }

  int add_node(int core_mask, int nodeid)
  {
    int retcode;
    int resp_sz;
    int quorum_id = choose_quorum(core_mask);
    while(true) {
      packet_out->code        = RPC_REQ_NODEADD;
      packet_out->flags       = 0;
      packet_out->client_id   = me;
      packet_out->core_mask     = core_mask;
      packet_out->client_port = me_queue;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = sizeof(cfg_change_t);
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node      = nodeid;
      send_to_server(packet_out, sizeof(rpc_t) + sizeof(cfg_change_t), quorum_id);
      resp_sz = common_receive_loop(sizeof(rpc_t) + sizeof(cfg_change_t));
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_FAIL) {
	continue;
      }
      break;
    }
    return 0;
  }

  int make_rpc(void *payload, int sz, void **response, int core_mask, int flags)
  {
    int retcode;
    int resp_sz;
    int quorum_id = choose_quorum(core_mask);
    while(true) {
      // Make request
      packet_out->code        = RPC_REQ;
      packet_out->flags       = flags;
      packet_out->client_id   = me;
      packet_out->core_mask     = core_mask;
      packet_out->client_port = me_queue;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = sz;
      memcpy(packet_out + 1, payload, sz);
      send_to_server(packet_out, sizeof(rpc_t) + sz, quorum_id);
      resp_sz = common_receive_loop(sizeof(rpc_t) + sz);
      if(resp_sz == -1) {
	update_server("rx timeout, make rpc");
	continue;
      }
      if(packet_in->code == RPC_REP_FAIL) {
	continue;
      }
      break;
    }
    *response = (void *)(packet_in + 1);
    return (int)(resp_sz - sizeof(rpc_t));
  }
} rpc_client_t;


void* cyclone_client_init(int client_id,
			  int client_mc,
			  int client_queue,
			  const char *config_cluster,
			  int server_ports,
			  const char *config_quorum)
{
  rpc_client_t * client = new rpc_client_t();
  boost::property_tree::ptree pt_cluster;
  boost::property_tree::ptree pt_quorum;
  boost::property_tree::read_ini(config_cluster, pt_cluster);
  boost::property_tree::read_ini(config_quorum, pt_quorum);
  std::stringstream key;
  std::stringstream addr;
  client->router = new quorum_switch(&pt_cluster, &pt_quorum);
  client->terms  = (unsigned int *)malloc(num_quorums*sizeof(unsigned int));
  client->me = client_id;
  client->me_mc = client_mc;
  client->me_queue = client_queue;
  client->buf = (dpdk_rx_buffer_t *)malloc(sizeof(dpdk_rx_buffer_t));
  client->buf->buffered = 0;
  client->buf->consumed = 0;
  client->server_ports = server_ports;
  void *buf = new char[MSG_MAXSIZE];
  client->packet_out = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_out_aux = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_in = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_rep = (msg_t *)buf;
  client->replicas = pt_quorum.get<int>("quorum.replicas");
  client->channel_seq = client_id*client_mc*rtc_clock::current_time();
  for(int i=0;i<num_quorums;i++) {
    client->server = 0;
    client->update_server("Initialization");
  }
  return (void *)client;
}

int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response,
	     int core_mask,
	     int flags)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  if(sz > DISP_MAX_MSGSIZE) {
    BOOST_LOG_TRIVIAL(fatal) << "rpc call params too large "
			     << " param size =  " << sz
			     << " DISP_MAX_MSGSIZE = " << DISP_MAX_MSGSIZE;
    exit(-1);
  }
  return client->make_rpc(payload, sz, response, core_mask, flags);
}

int delete_node(void *handle, int core_mask, int node)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->delete_node(core_mask, node);
}

int add_node(void *handle, int core_mask, int node)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->add_node(core_mask, node);
}
