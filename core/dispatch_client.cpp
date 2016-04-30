#include <stdlib.h>
#include "cyclone.hpp"
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include "../core/cyclone_comm.hpp"
#include "../core/logging.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <unistd.h>
#include "timeouts.hpp"

typedef struct rpc_client_st {
  int me;
  int me_mc;
  client_switch *router;
  rpc_t *packet_out;
  rpc_t *packet_in;
  int server;
  int replicas;
  unsigned long channel_seq;

  void update_server(const char *context)
  {
    BOOST_LOG_TRIVIAL(info) 
      << "CLIENT DETECTED POSSIBLE FAILED MASTER: "
      << server
      << " Reason " 
      << context;
    server = (server + 1)%replicas;
    BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW MASTER " << server;
  }

  void set_server()
  {
    BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING MASTER " << server;
  }

  int get_last_txid()
  {
    int retcode;
    int resp_sz;
    rtc_clock clock;
    while(true) {
      packet_out->code        = RPC_REQ_LAST_TXID;
      packet_out->client_id   = me;
#ifdef TRACING
      packet_out->timestamp   = clock.current_time();
#endif
      packet_out->client_txid = (int)packet_out->timestamp;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      retcode = cyclone_tx_timeout(router->output_socket(server), 
				   (unsigned char *)packet_out, 
				   sizeof(rpc_t), 
				   timeout_msec*1000,
				   "PROPOSE");
      if(retcode == -1) {
	update_server("tx timeout");
	continue;
      }
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}

	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout, get txid");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	if(packet_in->master == -1) {
	  BOOST_LOG_TRIVIAL(info) << "Unknown master !";
	  update_server("unknown master");
	}
	else {
	  server = packet_in->master;
	  set_server();
	}
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
    rtc_clock clock;
    while(true) {
      packet_out->code        = RPC_REQ_NODEDEL;
      packet_out->client_id   = me;
#ifdef TRACING
      packet_out->timestamp   = clock.current_time();
#endif
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node = nodeid;
      retcode = cyclone_tx_timeout(router->output_socket(server), 
				   (unsigned char *)packet_out, 
				   sizeof(rpc_t) + sizeof(cfg_change_t), 
				   timeout_msec*1000,
				   "PROPOSE");
      if(retcode == -1) {
	update_server("tx timeout");
	continue;
      }
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	if(packet_in->master == -1) {
	  BOOST_LOG_TRIVIAL(info) << "Unknown master !";
	  update_server("unknown master");
	}
	else {
	  server = packet_in->master;
	  set_server();
	}
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
    rtc_clock clock;
    while(true) {
      packet_out->code        = RPC_REQ_NODEADD;
      packet_out->client_id   = me;
#ifdef TRACING      
      packet_out->timestamp   = clock.current_time();
#endif
      packet_out->client_txid = txid;
      packet_out->channel_seq = channel_seq++;
      packet_out->requestor   = me_mc;
      cfg_change_t *cfg = (cfg_change_t *)(packet_out + 1);
      cfg->node      = nodeid;

      retcode = cyclone_tx_timeout(router->output_socket(server), 
				   (unsigned char *)packet_out, 
				   sizeof(rpc_t) + sizeof(cfg_change_t), 
				   timeout_msec*1000,
				   "PROPOSE");
      if(retcode == -1) {
	update_server("tx timeout");
	continue;
      }
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	if(packet_in->master == -1) {
	  BOOST_LOG_TRIVIAL(info) << "Unknown master !";
	  update_server("unknown master");
	}
	else {
	  server = packet_in->master;
	  set_server();
	}
	continue;
      }
      break;
    }
    return packet_in->last_client_txid;
  }

  int retrieve_response(void **response, int txid)
  {
    rtc_clock clock;
    int retcode;
    int resp_sz;
    packet_out->client_id   = me;
    packet_out->client_txid = txid;
#ifdef TRACING    
    packet_out->timestamp   = clock.current_time();
#endif
    packet_out->channel_seq  = channel_seq++;
    packet_out->requestor   = me_mc;
    while(true) {
      packet_out->code        = RPC_REQ_STATUS;
      retcode = cyclone_tx_timeout(router->output_socket(server), 
				   (unsigned char *)packet_out, 
				   sizeof(rpc_t), 
				   timeout_msec*1000,
				   "PROPOSE");
      if(retcode == -1) {
	update_server("tx timeout");
	continue;
      }

      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout, get response");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	if(packet_in->master == -1) {
	  BOOST_LOG_TRIVIAL(info) << "Unknown master !";
	  update_server("unknown master");
	}
	else {
	  server = packet_in->master;
	  set_server();
	}
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
    rtc_clock clock;
    while(true) {
      // Make request
      while(true) {
	packet_out->code        = RPC_REQ_FN;
	packet_out->flags       = flags;
	packet_out->client_id   = me;
	packet_out->client_txid = txid;
	packet_out->channel_seq = channel_seq++;
	packet_out->requestor   = me_mc;
#ifdef TRACING	
	packet_out->timestamp   = clock.current_time();
#endif
	memcpy(packet_out + 1, payload, sz);
	retcode = cyclone_tx_timeout(router->output_socket(server), 
				     (unsigned char *)packet_out, 
				     sizeof(rpc_t) + sz, 
				     timeout_msec*1000,
				     "PROPOSE");
	if(retcode == -1) {
	  update_server("tx timeout");
	  continue;
	}
	break;
      }
      while(true) {
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
	if(resp_sz == -1) {
	  break;
	}
	
	if(packet_in->channel_seq != (channel_seq - 1)) {
	  continue;
	}
	break;
      }
      if(resp_sz == -1) {
	update_server("rx timeout, make rpc");
	continue;
      }
      if(packet_in->code == RPC_REP_INVSRV) {
	if(packet_in->master == -1) {
	  BOOST_LOG_TRIVIAL(info) << "Unknown master !";
	  update_server("unknown master");
	}
	else {
	  server = packet_in->master;
	  set_server();
	}
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
} rpc_client_t;


void* cyclone_client_init(int client_id,
			  int client_mc,
			  int replicas,
			  int clients,
			  const char *config_server,
			  const char *config_client)
{
  rtc_clock clock;
  rpc_client_t * client = new rpc_client_t();
  client->me = client_id;
  client->me_mc = client_mc;
  boost::property_tree::ptree pt_server;
  boost::property_tree::ptree pt_client;
  boost::property_tree::read_ini(config_server, pt_server);
  boost::property_tree::read_ini(config_client, pt_client);
  void *zmq_context = zmq_init(1);
  client->router = new client_switch(zmq_context,
				     &pt_server,
				     &pt_client,
				     client_id,
				     client_mc,
				     clients,
				     false);
  void *buf = new char[DISP_MAX_MSGSIZE];
  client->packet_out = (rpc_t *)buf;
  buf = new char[DISP_MAX_MSGSIZE];
  client->packet_in = (rpc_t *)buf;
  client->server    = 0;
  client->replicas = replicas;
  client->channel_seq = clock.current_time();
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
  return client->make_rpc(payload, sz, response, txid, flags);
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
