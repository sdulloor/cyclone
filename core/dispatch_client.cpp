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
  client_switch *router;
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

  int get_last_txid()
  {
    int retcode;
    int resp_sz;
    rtc_clock clock;
    while(true) {
      packet_out->code        = RPC_REQ_LAST_TXID;
      packet_out->client_id   = me;
      packet_out->timestamp   = clock.current_time();
      packet_out->client_txid = (int)packet_out->timestamp;
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
	if(resp_sz != -1 &&
	   packet_in->code != RPC_REP_INVSRV &&
	   packet_in->client_txid != packet_out->client_txid) {
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
    if(packet_in->code == RPC_REP_OLD) {
      return RPC_EOLD;
    }
    return 0;
  }

  int delete_node(int txid, int nodeid)
  {
    int retcode;
    int resp_sz;
    rtc_clock clock;
    while(true) {
      packet_out->code        = RPC_REQ_NODEDEL;
      packet_out->client_id   = me;
      packet_out->timestamp   = clock.current_time();
      packet_out->client_txid = txid;
      packet_out->master      = nodeid;
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
	if(resp_sz != -1 &&
	   packet_in->code != RPC_REP_INVSRV &&
	   packet_in->client_txid != packet_out->client_txid) {
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
    packet_out->timestamp   = clock.current_time();
    while(true) {
      packet_out->code        = RPC_REQ_STATUS_BLOCK;
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
	if(resp_sz != -1 && 
	   packet_in->client_txid != txid &&
	   packet_in->code != RPC_REP_INVSRV) {
	  continue; // Ignore response
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
      if(packet_in->code == RPC_REP_PENDING) {
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
	packet_out->timestamp   = clock.current_time();
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
	while(true) {
	  resp_sz = cyclone_rx_timeout(router->input_socket(server), 
				       (unsigned char *)packet_in, 
				       DISP_MAX_MSGSIZE, 
				       timeout_msec*1000,
				       "RESULT");
	  if(resp_sz != -1 && 
	     packet_in->code != RPC_REP_INVSRV &&
	     packet_in->client_txid != txid) {
	    continue; // Ignore response
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

      while(true) {
	packet_out->code        = RPC_REQ_STATUS_BLOCK;
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
	  if(resp_sz != -1 && 
	     packet_in->client_txid != txid &&
	     packet_in->code != RPC_REP_INVSRV) {
	    continue; // Ignore response
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
	if(packet_in->code == RPC_REP_PENDING) {
	  continue;
	}
	break;
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
  rpc_client_t * client = new rpc_client_t();
  client->me = client_id;
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
