#include "cyclone.hpp"
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include "../core/cyclone_comm.hpp"
#include<boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <unistd.h>
#include "timeouts.hpp"

typedef struct rpc_client_st {
  int me;
  int ctr;
  cyclone_switch *router;
  rpc_t *packet_out;
  rpc_t *packet_in;
  int server;
  int replicas;
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

  int make_rpc(void *payload, int sz, void **response)
  {
    int retcode;
    int resp_sz;
    int txid;
    rtc_clock clock;
    while(true) {
      // Make request
      while(true) {
	packet_out->code        = RPC_REQ_FN;
	packet_out->client_id   = me;
	packet_out->client_txid = ctr;
	packet_out->timestamp   = clock.current_time();
	memcpy(packet_out + 1, payload, sz);
	txid = ctr;
	retcode = cyclone_tx_timeout(router->output_socket(server), 
				     (unsigned char *)packet_out, 
				     sizeof(rpc_t) + sz, 
				     timeout_msec*1000,
				     "PROPOSE");
	if(retcode == -1) {
	  update_server("tx timeout");
	  continue;
	}
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
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

      while(true) {
	packet_out->code        = RPC_REQ_STATUS_BLOCK;
	txid = ctr;
	retcode = cyclone_tx_timeout(router->output_socket(server), 
				     (unsigned char *)packet_out, 
				     sizeof(rpc_t), 
				     timeout_msec*1000,
				     "PROPOSE");
	if(retcode == -1) {
	  update_server("tx timeout");
	  continue;
	}
	resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				     (unsigned char *)packet_in, 
				     DISP_MAX_MSGSIZE, 
				     timeout_msec*1000,
				     "RESULT");
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
	if(packet_in->code == RPC_REP_PENDING) {
	  continue;
	}
	break;
      }

      if(packet_in->code == RPC_REP_REDO) {
	continue;
      }
      break;
    }
    *response = (void *)(packet_in + 1);
    return (int)(resp_sz - sizeof(rpc_t));
  }
} rpc_client_t;


void* cyclone_client_init(int client_id, const char *config)
{
  rpc_client_t * client = new rpc_client_t();
  client->me = client_id;
  boost::property_tree::ptree pt;
  boost::property_tree::read_ini(config, pt);
  void *zmq_context = zmq_init(1);
  int replicas = pt.get<int>("network.replicas");
  unsigned long server_port = pt.get<unsigned long>("dispatch.server_baseport");
  unsigned long client_port = pt.get<unsigned long>("dispatch.client_baseport");
  client->router = new cyclone_switch(zmq_context,
				      &pt,
				      client_id,
				      replicas,
				      client_port,
				      server_port,
				      false,
				      false);
  void *buf = new char[DISP_MAX_MSGSIZE];
  client->packet_out = (rpc_t *)buf;
  buf = new char[DISP_MAX_MSGSIZE];
  client->packet_in = (rpc_t *)buf;
  client->server    = 0;
  client->ctr = RPC_INIT_TXID;
  client->replicas = replicas;
  return (void *)client;
}

int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->make_rpc(payload, sz, response);
}
