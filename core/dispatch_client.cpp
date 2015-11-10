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

  // Check that the last successfully issued RPC has completed
  unsigned long await_completion()
  {
    unsigned long resp_sz;
    int retcode, e;
    while(true) {
      packet_out->code        = RPC_REQ_STATUS_BLOCK;
      packet_out->client_id   = me;
      packet_out->client_txid = ctr;
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
      if(packet_in->code == RPC_REP_COMPLETE) {
	ctr++;
	break;
      }
      else if(packet_in->code == RPC_REP_INVSRV) {
	server = packet_in->master;
	if(server == -1) { // Startup
	  BOOST_LOG_TRIVIAL(error) << "Unknown leader - assuming 0";
	  server = 0;
	}
	set_server();
      }
      else if(packet_in->code == RPC_REP_INVTXID) {
	BOOST_LOG_TRIVIAL(info)
	  << "CLIENT UPDATE TXID (completion) "
	  << ctr
	  << "->"
	  << packet_in->client_txid;
	ctr = packet_in->client_txid;
      }
    }
    return resp_sz;
  }

  int make_rpc(void *payload,
	       int sz,
	       void **response)
  {
    int retcode;
    int resp_sz;
    int txid;
    while(true) {
      packet_out->code      = RPC_REQ_FN;
      packet_out->client_id = me;
      packet_out->client_txid = ctr;
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

      if(packet_in->code == RPC_REP_INVTXID) {
	BOOST_LOG_TRIVIAL(info)
	  << "CLIENT UPDATE TXID "
	  << ctr
	  << "->"
	  << packet_in->client_txid;
	ctr = packet_in->client_txid;
	resp_sz = await_completion();
	continue;
      }
      else if(packet_in->code == RPC_REP_PENDING) {
	resp_sz = await_completion();
	if(ctr == (txid + 1)) {
	  break;
	}
	else {
	  continue;
	}
      }
      else if(packet_in->code == RPC_REP_INVSRV) {
	server = packet_in->master;
	if(server == -1) {
	  BOOST_LOG_TRIVIAL(error) << "Unknown leader - assuming 0";
	  server = 0;
	}
	set_server();
      }
      else if(packet_in->code == RPC_REP_COMPLETE) {
	ctr++;
	break;
      }
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
