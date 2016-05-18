#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <zmq.h>
#include <unistd.h>
#include "clock.hpp"

// Cyclone communication

static int cyclone_tx(void *socket,
		      const unsigned char *data,
		      unsigned long size,
		      const char *context) 
{
  int rc = zmq_send(socket, data, size, ZMQ_NOBLOCK);
  if(rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(warning) 
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

static int cyclone_tx_timeout(void *socket,
			      const unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs,
			      const char *context) 
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
  while (true) {
    rc = zmq_send(socket, data, size, ZMQ_NOBLOCK);
    if(rc >= 0) {
      break;
    }
    if(rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(warning) 
	  << "CYCLONE: Unable to transmit "
	  << context << " "
	  << zmq_strerror(zmq_errno());
	exit(-1);
      }
    }
    if((rtc_clock::current_time() - mark) >= timeout_usecs) {
      break;
    }
  }
  return rc;
}

static int cyclone_rx(void *socket,
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

static int cyclone_rx_timeout(void *socket,
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

static int cyclone_rx_noblock(void *socket,
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

static void * setup_cyclone_inpoll(void **sockets, int cnt)
{
  zmq_pollitem_t *items = new zmq_pollitem_t[cnt];
  for(int i=0;i<cnt;i++) {
    items[i].socket = sockets[i];
    items[i].events = ZMQ_POLLIN;
  }
  return items;
}

static void delete_cyclone_inpoll(void *pollitem)
{
  delete[] ((zmq_pollitem_t *)pollitem);
}

static int cyclone_poll(void * poll_handle, int cnt, int msec_timeout)
{
  zmq_pollitem_t *items = (zmq_pollitem_t *)poll_handle;
  int e = zmq_poll(items, cnt, msec_timeout);
  return e;
}

static int cyclone_socket_has_data(void *poll_handle, int index)
{
  zmq_pollitem_t *items = (zmq_pollitem_t *)poll_handle;
  return ((items[index].revents & ZMQ_POLLIN) != 0) ? 1:0;
}

static void socket_set_nolinger(void *socket)
{
  int linger = 0;
  int e = zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock linger "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
}

static void* cyclone_socket_out(void *context, bool use_hwm)
{
  void *socket;
  int hwm = 5;
  socket = zmq_socket(context, ZMQ_PUSH);
  if(use_hwm) {
    int e = zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(int));
    if (e == -1) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE_COMM: Unable to set sock HWM "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
  }
  socket_set_nolinger(socket);
  return socket;
}

static void* cyclone_socket_out_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REQ);
  socket_set_nolinger(socket);
  return socket;
}

static void* cyclone_socket_in(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_PULL);
  return socket;
}

static void* cyclone_socket_in_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REP);
  return socket;
}

static void cyclone_connect_endpoint(void *socket, const char *endpoint)
{
  BOOST_LOG_TRIVIAL(info)
    << "CYCLONE::COMM Connecting to "
    << endpoint;
  zmq_connect(socket, endpoint);
}

static void cyclone_bind_endpoint(void *socket, const char *endpoint)
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

class raft_switch {
  void **sockets_out;
  void **control_sockets_out;
  void *socket_in;
  void *request_socket_in;
  void *request_socket_out;
  void *control_socket_in;
  int replicas;
public:
  raft_switch(void *context,
	      boost::property_tree::ptree *pt,
	      int me,
	      int replicas_in,
	      bool use_hwm)
    :replicas(replicas_in)
  {
    std::stringstream key;
    std::stringstream addr;
    sockets_out = new void *[replicas];
    control_sockets_out = new void*[replicas];

    key.str("");key.clear();
    key << "quorum.baseport";
    int baseport =  pt->get<int>(key.str().c_str());

    // Create input socket
    socket_in = cyclone_socket_in(context);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "machines.iface" << me;
    addr << "tcp://";
    addr << pt->get<std::string>(key.str().c_str());
    int port = baseport + me;
    addr << ":" << port;
    cyclone_bind_endpoint(socket_in, addr.str().c_str());

    // Create input request socket
    request_socket_in = cyclone_socket_in_loopback(context);
    cyclone_bind_endpoint(request_socket_in, "inproc://RAFT_REQ");
    // Create output request socket
    request_socket_out = cyclone_socket_out_loopback(context);
    cyclone_connect_endpoint(request_socket_out, "inproc://RAFT_REQ");
    
    for(int i=0;i<replicas;i++) {
      sockets_out[i] = cyclone_socket_out(context, use_hwm);
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "machines.addr" << i;
      addr << "tcp://";
      addr << pt->get<std::string>(key.str().c_str());
      port = baseport + i ;
      addr << ":" << port;
      cyclone_connect_endpoint(sockets_out[i], addr.str().c_str());
    }
    control_socket_in = cyclone_socket_in_loopback(context);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "machines.iface" << me;
    addr << "tcp://";
    addr << pt->get<std::string>(key.str().c_str());
    port = baseport + replicas + me;
    addr << ":" << port;
    cyclone_bind_endpoint(control_socket_in, addr.str().c_str());
    for(int i=0;i<replicas;i++) {
      control_sockets_out[i] = cyclone_socket_out_loopback(context);
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "machines.addr" << i;
      addr << "tcp://";
      addr << pt->get<std::string>(key.str().c_str());
      port = baseport + replicas + i ;
      addr << ":" << port;
      cyclone_connect_endpoint(control_sockets_out[i], addr.str().c_str());
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
    for(int i=0;i<=replicas;i++) {
      zmq_close(sockets_out[i]);
      zmq_close(control_sockets_out[i]);
    }
    zmq_close(socket_in);
    zmq_close(request_socket_in);
    zmq_close(request_socket_out);
    zmq_close(control_socket_in);
    delete[] sockets_out;
  }
};

// A replica on each machine
// A version of every client on every machine
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

class server_switch {
  void *saved_context;
  void **sockets_out;
  int *socket_out_ports;
  void *socket_in;
  int client_machines;
  int server_machines;
  int clients;
  bool saved_use_hwm;
  boost::property_tree::ptree *saved_pt_client;

  int index(int mc, int client)
  {
    return mc*clients + client;
  }

  void **mux_ports;
  void *demux_port;

public:

  server_switch(void *context, 
		boost::property_tree::ptree *pt_server,
		boost::property_tree::ptree *pt_client,
		int me,
		int clients_in,
		bool use_hwm,
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
    client_machines =  pt_client->get<int>(key.str().c_str());
    server_machines =  pt_server->get<int>(key.str().c_str());
    
    clients  = clients_in;
    saved_use_hwm = use_hwm;
    saved_pt_client = pt_client;

    sockets_out = new void *[client_machines*clients];
    socket_out_ports = new int[client_machines*clients];

    // Input wire
    socket_in   = cyclone_socket_in(context); 
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "machines.iface" << me;
    addr << "tcp://";
    addr << pt_server->get<std::string>(key.str().c_str());
    port = server_baseport + me;
    addr << ":" << port;
    cyclone_bind_endpoint(socket_in, addr.str().c_str());
    for(int i=0;i<client_machines;i++) {
      for(int j=0;j<clients;j++) {
	sockets_out[index(i,j)] = NULL;
      }
    }
    mux_ports = new void *[mux_port_cnt];
    for(int i=0;i<mux_port_cnt;i++) {
      mux_ports[i] = cyclone_socket_out_loopback(context);
      cyclone_connect_endpoint(mux_ports[i], "inproc://MUXDEMUX");
    }
  }

  
  
  void ring_doorbell_backend(int mc,
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
    std::stringstream key; 
    std::stringstream addr;
    // output wire to client
    void *socket = cyclone_socket_out(saved_context, saved_use_hwm);
    sockets_out[index(mc, client)] = socket;
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "machines.addr" << mc;
    addr << "tcp://";
    addr << saved_pt_client->get<std::string>(key.str().c_str());
    addr << ":" << port;
    cyclone_connect_endpoint(socket, addr.str().c_str());
    socket_out_ports[index(mc, client)] = port;
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
    cyclone_tx(mux_ports[mux_index],
	       (const unsigned char *)&cmd,
	       sizeof(mux_state_t),
	       "mux");
    cyclone_rx(mux_ports[mux_index],
	       (unsigned char *)&ok,
	       sizeof(int),
	       "demux");
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
    
    cyclone_tx(mux_ports[mux_index],
	       (const unsigned char *)&cmd,
	       sizeof(mux_state_t),
	       "mux");
    cyclone_rx(mux_ports[mux_index],
	       (unsigned char *)&ok,
	       sizeof(int),
	       "demux");
  }

  void *input_socket()
  {
    return socket_in;
  }

  void operator() ()
  {
    mux_state_t cmd;
    int ok = 0;
    demux_port =
      cyclone_socket_in_loopback(saved_context);
    cyclone_bind_endpoint(demux_port, "inproc://MUXDEMUX");
    while(true) {
      cyclone_rx(demux_port, 
		 (unsigned char *)&cmd, 
		 sizeof(mux_state_t),
		 "demux");
      if(cmd.op == RING_DOORBELL) {
	ring_doorbell_backend(cmd.mc,
			      cmd.client,
			      cmd.port);
      }
      else {
	if(sockets_out[index(cmd.mc, cmd.client)] == NULL) {
	  BOOST_LOG_TRIVIAL(info) << "received rpc req without doorbell "
				  << " mc = " << cmd.mc
				  << " client = " << cmd.client;
	  exit(-1);
	}
	cyclone_tx(sockets_out[index(cmd.mc, cmd.client)],
		   (const unsigned char *)cmd.data,
		   cmd.size,
		   "demux");
      }
      cyclone_tx(demux_port, 
		 (unsigned char *)&ok, 
		 sizeof(int),
		 "demux");
    }
  }
  
  ~server_switch()
  {
    for(int i=0;i<client_machines;i++) {
      for(int j=0;j<clients;j++) {
	if(sockets_out[index(i, j)] != NULL) {
	  zmq_close(sockets_out[index(i, j)]);
	}
      }
    }
    zmq_close(socket_in);
    delete[] sockets_out;
   }
};

class client_switch {
  void **sockets_out;
  void **sockets_in;
  int *ports_in;
  int server_machines;
  int client_machines;

public:
  client_switch(void *context, 
		boost::property_tree::ptree *pt_server,
		boost::property_tree::ptree *pt_client,
		int me,
		int me_mc,
		bool use_hwm)
    
  {
    std::stringstream key; 
    std::stringstream addr;

    key.str("");key.clear();
    key << "dispatch.server_baseport";
    int server_baseport =  pt_server->get<int>(key.str().c_str());

    key.str("");key.clear();
    key << "machines.machines";
    server_machines = pt_server->get<int>(key.str().c_str());
    client_machines = pt_client->get<int>(key.str().c_str());

    sockets_in  = new void *[server_machines];
    ports_in    = new int[server_machines];
    sockets_out = new void *[server_machines];

    char * zmq_dsn = new char[1024];
    int port;
    for(int i=0;i<server_machines;i++) {
      // input wire from server 
      sockets_in[i] = cyclone_socket_in(context);
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "machines.iface" << me_mc;
      addr << "tcp://";
      addr << pt_client->get<std::string>(key.str().c_str());
      addr << ":*";
      cyclone_bind_endpoint(sockets_in[i], addr.str().c_str());
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
      // output wire to server
      sockets_out[i] = cyclone_socket_out(context, use_hwm);
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "machines.addr" << i;
      addr << "tcp://";
      addr << pt_server->get<std::string>(key.str().c_str());
      port = server_baseport + i;
      addr << ":" << port;
      cyclone_connect_endpoint(sockets_out[i], addr.str().c_str());
    }
    delete zmq_dsn;
  }
    
  void * output_socket(int machine)
  {
    return sockets_out[machine];
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
    for(int i=0;i<server_machines;i++) {
      zmq_close(output_socket(i));
      zmq_close(input_socket(i));
    }
    delete[] sockets_out;
    delete[] sockets_in;
  }
};

  
#endif
