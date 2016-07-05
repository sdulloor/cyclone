#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <zmq.h>
#include <unistd.h>
#include "clock.hpp"

/* Cyclone max message size */
const int MSG_MAXSIZE  = 2048;

#if defined(ZMQ_STACK)
#include "cyclone_comm_zmq.hpp"
#elif defined(DPDK_STACK)
#include "cyclone_comm_dpdk.hpp"
#else
#error "Need a network stack."
#endif

// Cyclone communication

static void* cyclone_socket_out_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REQ);
  int linger = 0;
  int e = zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock linger "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
  return socket;
}

static void* cyclone_socket_in_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REP);
  return socket;
}

static void cyclone_connect_endpoint_loopback(void *socket, const char *endpoint)
{
  BOOST_LOG_TRIVIAL(info)
    << "CYCLONE::COMM Connecting to "
    << endpoint;
  zmq_connect(socket, endpoint);
}

static void cyclone_bind_endpoint_loopback(void *socket, const char *endpoint)
{
  int rc = zmq_bind(socket, endpoint);
  if (rc != 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "CYCLONE::COMM Unable to setup listening socket at "
      << endpoint << " "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
  else {
    BOOST_LOG_TRIVIAL(info)
      << "CYCLONE::COMM Listening at "
      << endpoint;
  }
}

static int cyclone_tx_loopback(void *socket,
			       const unsigned char *data,
			       unsigned long size,
			       const char *context) 
{
  int rc = zmq_send(socket, data, size, ZMQ_DONTWAIT);
  if(rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE: Unable to transmit "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
    return -1;
  }
  else {
    return 0;
  }
}

// Keep trying until success
static void cyclone_tx_loopback_block(void *socket,
				      const unsigned char *data,
				      unsigned long size,
				      const char *context)
{
  int ok;
  do {
    ok = cyclone_tx_loopback(socket, data, size, context);
  } while(ok != 0);
}

// Block till data available
static int cyclone_rx_loopback_block(void *socket,
				     unsigned char *data,
				     unsigned long size,
				     const char *context)
{
  int rc;
  while (true) {
    rc = zmq_recv(socket, data, size, 0);
    if (rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(fatal) 
	  << "CYCLONE: Unable to receive "
	  << context << " "
	  << zmq_strerror(zmq_errno());
	exit(-1);
      }
      // Retry
    }
    else {
      break;
    }
  }
  return rc;
}

// Block till data available or timeout
static int cyclone_rx_loopback_timeout(void *socket,
				       unsigned char *data,
				       unsigned long size,
				       unsigned long timeout_usecs,
				       const char *context)
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
  while (true) {
    rc = zmq_recv(socket, data, size, ZMQ_NOBLOCK);
    if(rc >= 0) {
      break;
    }
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE: Unable to receive "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
    if((rtc_clock::current_time() - mark) >= timeout_usecs) {
      break;
    }
  }
  return rc;
}

// Best effort
static int cyclone_rx_loopback(void *socket,
			       unsigned char *data,
			       unsigned long size,
			       const char *context)
{
  int rc;
  rc = zmq_recv(socket, data, size, ZMQ_NOBLOCK);
  if (rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE: Unable to receive "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
  }
  return rc;
}

struct client_paths {
  void *saved_context;
  void **sockets_out;
  int *socket_out_ports;
  int clients;
  int client_machines;
  boost::property_tree::ptree *saved_pt_client;

  int index(int mc, int client)
  {
    return mc*clients + client;
  }

  void ring_doorbell(int mc,
		     int client,
		     int port)
  {

    if(sockets_out[index(mc, client)] != NULL && 
       socket_out_ports[index(mc, client)] == port) {
      return; // Already setup
    }
    if(sockets_out[index(mc, client)] != NULL) {
      zmq_close(sockets_out[index(mc, client)]);
    }
    // output wire to client
    void *socket = cyclone_socket_out(saved_context);
    sockets_out[index(mc, client)] = socket;
#if defined(DPDK_STACK)
    cyclone_connect_endpoint(socket, mc, q_dispatcher, saved_pt_client);
#else
    cyclone_connect_endpoint(socket, mc, port, saved_pt_client);
#endif
    socket_out_ports[index(mc, client)] = port;
  }

  void *socket(int mc, int client)
  {
    return sockets_out[index(mc, client)];
  }
  
  int client_port(int mc, int client)
  {
    return socket_out_ports[index(mc, client)];
  }

  void init()
  {
    sockets_out = new void *[client_machines*clients];
    socket_out_ports = new int[client_machines*clients];
    for(int i=0;i<client_machines;i++) {
      for(int j=0;j<clients;j++) {
	sockets_out[index(i,j)] = NULL;
	socket_out_ports[index(i,j)] = -1;
      }
    }
  }

  void teardown()
  {
    // TBD
  }
  
};

class raft_switch {
  void **sockets_out;
  void **control_sockets_out;
  void *socket_in;
  void *request_socket_in;
  void *request_socket_out;
  void *control_socket_in;
  int replicas;
public:
  client_paths cpaths;
  raft_switch(void *context,
	      void *loopback_context,
	      boost::property_tree::ptree *pt,
	      int me,
	      int replicas_in,
	      int clients_in,
	      boost::property_tree::ptree *pt_client)
    :replicas(replicas_in)
  {
    std::stringstream key;
    std::stringstream addr;

    sockets_out = new void *[replicas];
    control_sockets_out = new void*[replicas];


    key.str("");key.clear();
    key << "machines.machines";
    cpaths.client_machines =  pt_client->get<int>(key.str().c_str());
    
    cpaths.clients  = clients_in;
    cpaths.saved_pt_client = pt_client;
    cpaths.saved_context = context;

    cpaths.init();
    
    key.str("");key.clear();
    key << "quorum.baseport";
    int baseport =  pt->get<int>(key.str().c_str());

    // Create input socket
    socket_in = cyclone_socket_in(context);
    int port = baseport + me;
#if defined(DPDK_STACK)
    cyclone_bind_endpoint(socket_in, me, q_raft, pt);
#else
    cyclone_bind_endpoint(socket_in, me, port, pt);
#endif
    // Create input request socket
    request_socket_in = cyclone_socket_in_loopback(loopback_context);
    cyclone_bind_endpoint_loopback(request_socket_in, "inproc://RAFT_REQ");
    // Create output request socket
    request_socket_out = cyclone_socket_out_loopback(loopback_context);
    cyclone_connect_endpoint_loopback(request_socket_out, "inproc://RAFT_REQ");
    
    for(int i=0;i<replicas;i++) {
      sockets_out[i] = cyclone_socket_out(context);
      port = baseport + i ;
#if defined(DPDK_STACK)
      cyclone_connect_endpoint(sockets_out[i], i, q_raft, pt);
#else
      cyclone_connect_endpoint(sockets_out[i], i, port, pt);
#endif
    }
    control_socket_in = cyclone_socket_in_loopback(loopback_context);
    port = baseport + replicas + me;
#if defined(DPDK_STACK)
    cyclone_bind_endpoint(control_socket_in, me, q_control, pt);
#else
    cyclone_bind_endpoint(control_socket_in, me, port, pt);
#endif
    for(int i=0;i<replicas;i++) {
      control_sockets_out[i] = cyclone_socket_out_loopback(loopback_context);
      port = baseport + replicas + i ;
#if defined(DPDK_STACK)
      cyclone_connect_endpoint(control_sockets_out[i], i, q_control, pt);
#else
      cyclone_connect_endpoint(control_sockets_out[i], i, port, pt);
#endif
    }

  }

  void * output_socket(int machine)
  {
    return sockets_out[machine];
  }

  void* input_socket()
  {
    return socket_in;
  }

  void * request_in()
  {
    return request_socket_in;
  }

  void *request_out()
  {
    return request_socket_out;
  }
  
  void* control_input_socket()
  {
    return control_socket_in;
  }

  void * control_output_socket(int machine)
  {
    return control_sockets_out[machine];
  }

  ~raft_switch()
  {
    // TBD
  }
};

const int RING_DOORBELL = 0;
const int SEND_DATA     = 1;
typedef struct mux_state_st{
  int op;
  int mc;
  int client;
  // Doorbell
  int port;
  // Send data
  void *data;
  int size;
} mux_state_t;

// A replica on each machine
// A version of every client on every machine
class server_switch {
  void *saved_context;
  void *socket_in;
  int clients;
  client_paths cpaths;
  void **mux_ports;
  void *demux_port;

public:

  server_switch(void *context,
		void *loopback_context,
		boost::property_tree::ptree *pt_server,
		boost::property_tree::ptree *pt_client,
		int me,
		int clients_in,
		int mux_port_cnt)
    :saved_context(context)
    
  {
    std::stringstream key; 
    std::stringstream addr;
    int port;

    key.str("");key.clear();
    key << "dispatch.server_baseport";
    int server_baseport =  pt_server->get<int>(key.str().c_str());

    key.str("");key.clear();
    key << "machines.machines";
    cpaths.client_machines =  pt_client->get<int>(key.str().c_str());
    
    cpaths.clients  = clients_in;
    cpaths.saved_pt_client = pt_client;
    cpaths.saved_context = saved_context;
    
    cpaths.init();

    // Input wire
    socket_in   = cyclone_socket_in(context); 
    port = server_baseport + me;
#if defined(DPDK_STACK)
    cyclone_bind_endpoint(socket_in, me, q_dispatcher, pt_server);
#else
      cyclone_bind_endpoint(socket_in, me, port, pt_server);
#endif
    mux_ports = new void *[mux_port_cnt];
    for(int i=0;i<mux_port_cnt;i++) {
      mux_ports[i] = cyclone_socket_out_loopback(loopback_context);
      cyclone_connect_endpoint_loopback(mux_ports[i], "inproc://MUXDEMUX");
    }
  }

  
  
  void send_data(int mc,
		 int client,
		 void *data,
		 int size,
		 int mux_index)
  {
    mux_state_t cmd;
    int ok;
    cmd.op = SEND_DATA;
    cmd.mc = mc;
    cmd.client = client;
    cmd.data = data;
    cmd.size = size;
    cyclone_tx_loopback_block(mux_ports[mux_index],
			      (const unsigned char *)&cmd,
			      sizeof(mux_state_t),
			      "mux data");
    cyclone_rx_loopback_block(mux_ports[mux_index],
			      (unsigned char *)&ok,
			      sizeof(int),
			      "demux data");
  }

  void ring_doorbell(int mc,
		     int client,
		     int port,
		     int mux_index)
  {
    mux_state_t cmd;
    int ok;
    cmd.op = RING_DOORBELL;
    cmd.mc = mc;
    cmd.client = client;
    cmd.port = port;
    cyclone_tx_loopback_block(mux_ports[mux_index],
		     (const unsigned char *)&cmd,
		     sizeof(mux_state_t),
		     "mux doorbell");
    
    cyclone_rx_loopback_block(mux_ports[mux_index],
			      (unsigned char *)&ok,
			      sizeof(int),
			      "demux doorbell");
  }

  void *input_socket()
  {
    return socket_in;
  }

  int client_port(int mc, int client)
  {
    return cpaths.client_port(mc, client);
  }

  void operator() ()
  {
    mux_state_t cmd;
    int ok = 0;
    demux_port =
      cyclone_socket_in_loopback(saved_context);
    cyclone_bind_endpoint_loopback(demux_port, "inproc://MUXDEMUX");
    while(true) {
      cyclone_rx_loopback_block(demux_port, 
				(unsigned char *)&cmd, 
				sizeof(mux_state_t),
				"demux");
      if(cmd.op == RING_DOORBELL) {
	cpaths.ring_doorbell(cmd.mc,
			     cmd.client,
			     cmd.port);
      }
      else {
	cyclone_tx_loopback(cpaths.socket(cmd.mc, cmd.client),
			    (const unsigned char *)cmd.data,
			    cmd.size,
			    "demux data tx");
      }
      cyclone_tx_loopback_block(demux_port, 
				(unsigned char *)&ok, 
				sizeof(int),
				"demux resp");
    }
  }
  
  ~server_switch()
  {
    // TBD
   }
};

class client_switch {
  void **sockets_out;
  void **raft_sockets_out;
  void **sockets_in;
  int *ports_in;
  int server_machines;
  int client_machines;

public:
  client_switch(void *context, 
		boost::property_tree::ptree *pt_server,
		boost::property_tree::ptree *pt_client,
		int me,
		int me_mc)
    
  {
    std::stringstream key; 
    std::stringstream addr;

    key.str("");key.clear();
    key << "dispatch.server_baseport";
    int server_baseport =  pt_server->get<int>(key.str().c_str());
    int raft_baseport = pt_server->get<int>("quorum.baseport");
    key.str("");key.clear();
    key << "machines.machines";
    server_machines = pt_server->get<int>(key.str().c_str());
    client_machines = pt_client->get<int>(key.str().c_str());

    sockets_in  = new void *[server_machines];
    ports_in    = new int[server_machines];
    sockets_out = new void *[server_machines];
    raft_sockets_out = new void *[server_machines];
    
    char * zmq_dsn = new char[1024];
    int port;
    for(int i=0;i<server_machines;i++) {
      // input wire from server 
      sockets_in[i] = cyclone_socket_in(context);
#if defined(DPDK_STACK)
      cyclone_bind_endpoint(sockets_in[i], me_mc, q_dispatcher, pt_client);
#else
      cyclone_bind_endpoint(sockets_in[i], me_mc, -1, pt_client);
      size_t sz  = 1024;
      int e = zmq_getsockopt(sockets_in[i], ZMQ_LAST_ENDPOINT, zmq_dsn, &sz);
      if(e != 0) {
	BOOST_LOG_TRIVIAL(fatal) << "Unable to read port number";
	exit(-1);
      }
      BOOST_LOG_TRIVIAL(info) << "Bind point is " << zmq_dsn;
      char *ptr = &zmq_dsn[strlen(zmq_dsn) - 1];
      while((*ptr) != ':') ptr--;
      ptr++;
      ports_in[i] = atoi(ptr);
#endif
      // output wire to server
      sockets_out[i] = cyclone_socket_out(context);
      port = server_baseport + i;
#if defined(DPDK_STACK)
      cyclone_connect_endpoint(sockets_out[i], i, q_dispatcher, pt_server);
#else
      cyclone_connect_endpoint(sockets_out[i], i, port, pt_server);
#endif
      // output wire to raft instance
      raft_sockets_out[i] = cyclone_socket_out(context);
      port = raft_baseport + i ;
#if defined(DPDK_STACK)
      cyclone_connect_endpoint(raft_sockets_out[i], i, q_raft, pt_server);
#else
      cyclone_connect_endpoint(raft_sockets_out[i], i, port, pt_server);
#endif
    }
    delete zmq_dsn;
  }
    
  void * output_socket(int machine)
  {
    return sockets_out[machine];
  }

  void * raft_output_socket(int machine)
  {
    return raft_sockets_out[machine];
  }
  
  int input_port(int machine)
  {
    return ports_in[machine];
  }
  
  void *input_socket(int machine)
  {
    return sockets_in[machine];
  }

  ~client_switch()
  {
    // TBD
  }
};

  
#endif
