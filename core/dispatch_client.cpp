#include <stdlib.h>
#include "cyclone.hpp"
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include "../core/cyclone_comm.hpp"
#include "../core/logging.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <unistd.h>
#include "tuning.hpp"
#include "cyclone_context.hpp"


static volatile int threads = 0;

class cyclic {
  unsigned long *permutation;
  unsigned long cyclic_index;
  unsigned long machines;
  struct drand48_data rand_buffer;
public:
  cyclic(unsigned long machines_in, unsigned long seed)
    : machines(machines_in) {
    srand48_r(seed, &rand_buffer);
    permutation = new unsigned long[machines];
    permutation[0] = 0;
    for (unsigned long i = 1; i < machines; i++) {
      permutation[i] = i;
      double r;
      drand48_r(&rand_buffer, &r);
      unsigned long interchange = (unsigned long) (r * (i + 1));
      unsigned long tmp = permutation[interchange];
      permutation[interchange] = permutation[i];
      permutation[i] = tmp;
    }
    cyclic_index = 0;
  }

  unsigned long cyclic_next() {
    unsigned long mc = permutation[cyclic_index];
    cyclic_index = (cyclic_index + 1) % machines;
    return mc;
  }

  unsigned long cycle_size() {
    return machines;
  }

  ~cyclic() {
    delete permutation;
  }
};



typedef struct rpc_client_st {
  int me;
  int me_mc;
  client_switch *router;
  rpc_t *packet_out;
  msg_t *packet_rep;
  rpc_t *packet_in;
  int server;
  int replicas;
  unsigned long channel_seq;
  class cyclic *rep_order;
  void* network_context;
  const char *config_server;
  const char *config_client;
  int thread;

  void update_server(const char *context)
  {
    BOOST_LOG_TRIVIAL(info) 
      << "CLIENT DETECTED POSSIBLE FAILED LEADER: "
      << server
      << " Reason " 
      << context;
    server = (server + 1)%replicas;
    BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW LEADER " << server;
  }

  void set_server()
  {
    BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING LEADER " << server;
  }


  int common_receive_loop(int blob_sz)
  {
    int retcode;
    int resp_sz;
    bool sent_assist_msg   = false;
    bool sent_assist_reply = false;
    unsigned long response_map = 0;
    while(true) {
      resp_sz = cyclone_rx_timeout(router->input_socket(server),
				   (unsigned char *)packet_in,
				   MSG_MAXSIZE,
				   timeout_msec*1000,
				   "recv loop");
      if(resp_sz == -1) {
	break;
      }

      if(packet_in->channel_seq != (channel_seq - 1)) {
	continue;
      }

      if(packet_in->code == RPC_REQ_ASSIST) {
	if(sent_assist_msg) {
	  continue;
	}
	packet_rep->msg_type    = MSG_ASSISTED_APPENDENTRIES;
	packet_rep->client_port = packet_out->client_port; 
	memcpy(&packet_rep->rep, &packet_in->rep, sizeof(replicant_t));
	packet_rep->rep.client_id = me;
	packet_rep->rep.client_mc = me_mc;
	packet_rep->rep.channel_seq = channel_seq - 1;
	memcpy(packet_rep + 1,
	       packet_out,
	       blob_sz);
	// best effort
	for(int i = 0; i < replicas; i++) {
	  int replica = rep_order->cyclic_next();
	  if(replica == server) {
	    continue;
	  }
	  cyclone_tx_rand_queue(router->raft_output_socket(replica),
				(unsigned char *)packet_rep,
				sizeof(msg_t) + blob_sz,
				"ASSIST");
	}
	sent_assist_msg = true;
	continue;
      }

      if(packet_in->code == RPC_REP_ASSIST_OK) {
	if(sent_assist_reply) {
	  continue;
	}
	response_map = response_map | (1 << packet_in->requestor);
	int replicated_at = __builtin_popcount(response_map) + 1;
	//if(replicated_at > (replicas/2)) { // Majority quorum ?
	if(replicated_at == (replicas - 1)) { // Full quorum ?
	  packet_rep->msg_type = MSG_ASSISTED_QUORUM_OK;
	  packet_rep->quorum = response_map;
	  memcpy(&packet_rep->rep, &packet_in->rep, sizeof(replicant_t));
	  cyclone_tx_rand_queue(router->raft_output_socket(server),
				(unsigned char *)packet_rep,
				sizeof(msg_t),
				"ASSIST QUORUM");
	  sent_assist_reply = true;
	}
	continue;
      }
      break;
    }
    return resp_sz;
  }

  int get_last_txid()
  {
    int retcode;
    int resp_sz;
    while(true) {
      packet_out->code        = RPC_REQ_LAST_TXID;
      packet_out->client_id   = me;
      packet_out->client_port = router->input_port(server);
      packet_out->client_txid = (int)packet_out->timestamp;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = 0;
      retcode = cyclone_tx_queue(router->output_socket(server), 
				 (unsigned char *)packet_out, 
				 sizeof(rpc_t),
				 q_dispatcher,
				 "PROPOSE");
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server),
				   (unsigned char *)packet_in,
				   MSG_MAXSIZE,
				   timeout_msec*1000,
				   "recv loop");

	if(resp_sz == -1) {
	  break;
	}

	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}

	if(packet_in->code == RPC_REQ_ASSIST) {
	  continue;
	}
	
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout, get txid");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      break;
    }
    return packet_in->last_client_txid;
  }

  int delete_node(int txid, int nodeid)
  {
    int retcode;
    int resp_sz;
    while(true) {
      packet_out->code        = RPC_REQ_NODEDEL;
      packet_out->client_id   = me;
      packet_out->client_port = router->input_port(server);
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node = nodeid;
      retcode = cyclone_tx(router->output_socket(server), 
			   (unsigned char *)packet_out, 
			   sizeof(rpc_t) + sizeof(cfg_change_t), 
			   "PROPOSE");
      resp_sz = common_receive_loop(sizeof(rpc_t) + sizeof(cfg_change_t));
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      break;
    }
    return packet_in->last_client_txid;
  }

  int add_node(int txid, int nodeid)
  {
    int retcode;
    int resp_sz;
    while(true) {
      packet_out->code        = RPC_REQ_NODEADD;
      packet_out->client_id   = me;
      packet_out->client_port = router->input_port(server);
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node      = nodeid;
      retcode = cyclone_tx(router->output_socket(server), 
			   (unsigned char *)packet_out, 
			   sizeof(rpc_t) + sizeof(cfg_change_t), 
			   "PROPOSE");
      resp_sz = common_receive_loop(sizeof(rpc_t) + sizeof(cfg_change_t));
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      break;
    }
    return packet_in->last_client_txid;
  }

  int retrieve_response(void **response, int txid)
  {
    int retcode;
    int resp_sz;
    packet_out->client_id   = me;
    packet_out->client_port = router->input_port(server);
    packet_out->client_txid = txid;
    packet_out->channel_seq  = channel_seq++;
    packet_out->requestor   = me_mc;
    while(true) {
      packet_out->code        = RPC_REQ_STATUS;
      retcode = cyclone_tx(router->output_socket(server), 
			   (unsigned char *)packet_out, 
			   sizeof(rpc_t), 
			   "PROPOSE");
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server),
				     (unsigned char *)packet_in,
				     MSG_MAXSIZE,
				     timeout_msec*1000,
				     "recv loop");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}

	if(packet_in->code == RPC_REQ_ASSIST) {
	  continue;
	}
	
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout, get response");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      break;
    }
    if(packet_in->code == RPC_REP_OLD) {
      return RPC_EOLD;
    }
    if(packet_in->code == RPC_REP_UNKNOWN) {
      return RPC_EUNKNOWN;
    }
    *response = (void *)(packet_in + 1);
    return (int)(resp_sz - sizeof(rpc_t));
  }
  
  int make_rpc(void *payload, int sz, void **response, int txid, int flags)
  {
    int retcode;
    int resp_sz;
    while(true) {
      // Make request
      packet_out->code        = RPC_REQ_FN;
      packet_out->flags       = flags;
      packet_out->client_id   = me;
      packet_out->client_port = router->input_port(server);
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = sz;
      memcpy(packet_out + 1, payload, sz);
      retcode = cyclone_tx_queue(router->output_socket(server), 
				 (unsigned char *)packet_out, 
				 sizeof(rpc_t) + sz, 
				 q_dispatcher,
				 "PROPOSE");
      resp_sz = common_receive_loop(sizeof(rpc_t) + sz);
      if(resp_sz == -1) {
	update_server("rx timeout, make rpc");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      if(packet_in->code == RPC_REP_UNKNOWN) {
	continue;
      }
      break;
    }
    if(packet_in->code == RPC_REP_OLD) {
      return RPC_EOLD;
    }
    *response = (void *)(packet_in + 1);
    return (int)(resp_sz - sizeof(rpc_t));
  }

  int make_noop_rpc(int txid, int flags)
  {
    int retcode;
    int resp_sz;
    while(true) {
      // Make request
      packet_out->code        = RPC_REQ_NOOP;
      packet_out->flags       = flags;
      packet_out->client_id   = me;
      packet_out->client_port = router->input_port(server);
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      packet_out->payload_sz  = 0;
      retcode = cyclone_tx_queue(router->output_socket(server), 
				 (unsigned char *)packet_out, 
				 sizeof(rpc_t), 
				 q_dispatcher,
				 "PROPOSE");
      resp_sz = common_receive_loop(sizeof(rpc_t));
      if(resp_sz == -1) {
	update_server("rx timeout, make rpc");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	update_server("Server not leader");
	continue;
      }
      if(packet_in->code == RPC_REP_UNKNOWN) {
	continue;
      }
      break;
    }
    if(packet_in->code == RPC_REP_OLD) {
      return RPC_EOLD;
    }
    return 0;
  }
  
} rpc_client_t;


static void * network_context = NULL;

void cyclone_client_global_init(int threads)
{
#if defined(DPDK_STACK)
  network_context = dpdk_context_client(threads);
#else
  network_context = zmq_init(1);
#endif

}

void* cyclone_client_init(int client_id,
			  int client_mc,
			  int replicas,
			  const char *config_server,
			  const char *config_client)
{
  rpc_client_t * client = new rpc_client_t();
  client->thread = threads++;
  client->me = client_id;
  client->me_mc = client_mc;
  boost::property_tree::ptree pt_server;
  boost::property_tree::ptree pt_client;
  boost::property_tree::read_ini(config_server, pt_server);
  boost::property_tree::read_ini(config_client, pt_client);
  client->router = new client_switch(network_context,
				     &pt_server,
				     &pt_client,
				     client_id,
				     client_mc,
				     num_queues + client->thread);
  client->network_context = network_context;
  client->config_server = config_server;
  client->config_client = config_client;
  if(network_context == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Network context not initialized!\n";
    exit(-1);
  }
  void *buf = new char[MSG_MAXSIZE];
  client->packet_out = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_in = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_rep = (msg_t *)buf;
  client->replicas = replicas;
  client->channel_seq = client_id*client_mc*rtc_clock::current_time();
  client->server = 0;
  client->set_server();
  client->rep_order = new cyclic(client->replicas, client->me);
  return (void *)client;
}

void* cyclone_client_dup(void *handle, int me)
{
  rpc_client_t *orig = (rpc_client_t *)handle;
  rpc_client_t * client = new rpc_client_t();
  client->thread = threads++;
  client->me = me;
  client->me_mc = orig->me_mc;
  client->network_context = orig->network_context;
  client->config_server = orig->config_server;
  client->config_client = orig->config_client;
  boost::property_tree::ptree pt_server;
  boost::property_tree::ptree pt_client;
  boost::property_tree::read_ini(client->config_server, pt_server);
  boost::property_tree::read_ini(client->config_client, pt_client);
  client->router = new client_switch(client->network_context,
				     &pt_server,
				     &pt_client,
				     client->me,
				     client->me_mc,
				     num_queues + client->thread);
  void *buf = new char[MSG_MAXSIZE];
  client->packet_out = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_in = (rpc_t *)buf;
  buf = new char[MSG_MAXSIZE];
  client->packet_rep = (msg_t *)buf;
  client->replicas = orig->replicas;
  client->channel_seq = client->me*client->me_mc*rtc_clock::current_time();
  client->server = 0;
  client->set_server();
  client->rep_order = new cyclic(client->replicas, client->me);
  return (void *)client;
}

int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response,
	     int txid,
	     int flags)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  if(sz > DISP_MAX_MSGSIZE) {
    BOOST_LOG_TRIVIAL(fatal) << "rpc call params too large "
			     << " param size =  " << sz
			     << " DISP_MAX_MSGSIZE = " << DISP_MAX_MSGSIZE;
    exit(-1);
  }
  return client->make_rpc(payload, sz, response, txid, flags);
}

int make_noop_rpc(void *handle,
		  int txid,
		  int flags)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->make_noop_rpc(txid, flags);
}

int get_last_txid(void *handle)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->get_last_txid();
}

int get_response(void *handle, void **response, int txid)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->retrieve_response(response, txid);
}

int delete_node(void *handle, int txid, int node)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->delete_node(txid, node);
}

int add_node(void *handle, int txid, int node)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->add_node(txid, node);
}
