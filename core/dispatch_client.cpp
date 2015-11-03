#include "libcyclone.hpp"

typedef struct rpc_client_st {
  int me;
  int ctr;
  cyclone_switch *router;
  rpc_t *packet_out;
  rpc_t *packet_in;
  unsigned long server;
  void *poll_item;
  int replicas;
  void update_server(void **pll, cyclone_switch *router)
  {
    server = (server + 1)%replicas;
    delete_cyclone_inpoll(*pll);
    void *socket = router->input_socket(new_server);
    *pll = setup_cyclone_inpoll(&socket, 1);
    BOOST_LOG_TRIVIAL(info) << "CLIENT DETECTED POSSIBLE FAILED MASTER";
    BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW MASTER " << server;
  }

  unsigned long await_completion()
  {
    unsigned long resp_sz;
    while(true) {
      packet_out->code        = RPC_REQ_STATUS;
      packet_out->client_id   = me;
      packet_out->client_txid = ctr;
      int retcode = cyclone_tx(router->output_socket(server), 
			       (unsigned char *)packet_out, 
			       sizeof(rpc_t) + 12, 
			       "PROPOSE");
      if(retcode == -1) {
	update_server(&poll_item, router, server);
	continue;
      }
      do {
	e = cyclone_poll(poll_item, 1, 10000);
      } while( e < 0 && errno == EINTR); 
      if(cyclone_socket_has_data(poll_item, 0)) {
	resp_sz = cyclone_rx(router->input_socket(server), 
			     (unsigned char *)packet_in, 
			     DISP_MAX_MSGSIZE, 
			     "RESULT");
      }
      else {
	update_server(&poll_item, router, server);
	continue;
      }
      if(packet_in->code == RPC_REP_COMPLETE) {
	break;
      }
      else if(packet_in->code == RPC_REP_INVSRV) {
	server = packet_in->master;
	update_server(&poll_item, router, server);
	BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING MASTER " << server;
      }
      else if(packet_in->code == RPC_REP_INVTXID) {
	BOOST_LOG_TRIVIAL(info)
	  << "CLIENT UPDATE TXID "
	  << ctr
	  << "->"
	  << packet_in->client_txid;
	ctr = packet_in->client_txid;
      }
    }
    return resp_sz;
  }

  unsigned long make_rpc(void *payload,
			 int sz,
			 void **response,
			 int *response_size)
  {
    int retcode;
    unsigned long resp_sz;
    while(true) {
      packet_out->code      = RPC_REQ_FN;
      packet_out->client_id = me;
      packet_out->client_txid = ctr;
      memcpy(packet_out + 1, payload, sz);
      retcode = cyclone_tx(router->output_socket(server), 
			   (unsigned char *)packet_out, 
			   sizeof(rpc_t) + 12, 
			   "PROPOSE");
      if(retcode == -1) {
	update_server(&poll_item, router, server);
	continue;
      }
      int e;
      do {
	e = cyclone_poll(poll_item, 1, 10000);
      } while(e < 0 && errno == EINTR);
      if(cyclone_socket_has_data(poll_item, 0)) {
	cyclone_rx(router->input_socket(server), 
		   (unsigned char *)packet_in, 
		   DISP_MAX_MSGSIZE, 
		   "RESULT");
      }
      else {
	update_server(&poll_item, router, server);
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
	ctr++;
	continue;
      }
      else if(packet_in->code == RPC_REP_PENDING) {
	resp_sz = await_completion();
	ctr++;
	break;
      }
      else if(packet_in->code == RPC_REP_INVSRV) {
	server = packet_in->master;
	update_server(&poll_item, router, server);
	BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING MASTER " << server;
      }
    }
    *response = (void *)(packet_in + 1);
    return resp_sz - sizeof(rpc_t);
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
				      client_me,
				      replicas,
				      client_port,
				      server_port,
				      false);
  void *buf = new char[DISP_MAX_MSGSIZE];
  packet_out = (rpc_t *)buf;
  buf = new char[DISP_MAX_MSGSIZE];
  packet_in = (rpc_t *)buf;
  client->server    = 0;
  client->poll_item = setup_cyclone_inpoll(&sock, 1);
  client->ctr = RPC_INIT_TXID;
  client->replicas = replicas;
  return (void *)client;
}

unsigned long make_rpc(void *handle,
		       void *payload,
		       int sz,
		       void **response)
{
  rpc_client_t *client = (rpc_client_t *)handle;
  return client->make_rpc(payload, sz, response);
}
