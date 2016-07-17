#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <zmq.h>
#include <unistd.h>
#include "clock.hpp"
#include "runq.hpp"

/* Cyclone max message size */
const int MSG_MAXSIZE  = 700; // Maximum user data in pkt

#if defined(ZMQ_STACK)
#include "cyclone_comm_zmq.hpp"
#elif defined(DPDK_STACK)
#include "cyclone_comm_dpdk.hpp"
#else
#error "Need a network stack."
#endif

// Cyclone communication

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
#if defined(DPDK_STACK)
      socket_out_ports[index(mc, client)] = port;
      // No need to update socket on a change of port
      return;
#else
      zmq_close(sockets_out[index(mc, client)]);
#endif
    }
    // output wire to client
    void *socket = cyclone_socket_out(saved_context);
    sockets_out[index(mc, client)] = socket;
#if defined(DPDK_STACK)
    cyclone_connect_endpoint(socket, mc, q_client, saved_pt_client);
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
  void *control_socket_in;
  int replicas;
public:
  client_paths cpaths;
  raft_switch(void *context,
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
    for(int i=0;i<replicas;i++) {
      sockets_out[i] = cyclone_socket_out(context);
      port = baseport + i ;
#if defined(DPDK_STACK)
      cyclone_connect_endpoint(sockets_out[i], i, q_raft, pt);
#else
      cyclone_connect_endpoint(sockets_out[i], i, port, pt);
#endif
    }
    control_socket_in = cyclone_socket_in(context);
    port = baseport + replicas + me;
#if defined(DPDK_STACK)
    cyclone_bind_endpoint(control_socket_in, me, q_control, pt);
#else
    cyclone_bind_endpoint(control_socket_in, me, port, pt);
#endif
    for(int i=0;i<replicas;i++) {
      control_sockets_out[i] = cyclone_socket_out(context);
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
  struct mux_state_st * volatile next_issue;
  volatile int complete;
} mux_state_t;

// A replica on each machine
// A version of every client on every machine
class server_switch {
  void *saved_context;
  void *socket_in;
  int clients;
  client_paths cpaths;
  runq_t<mux_state_t> muxq;

public:

  server_switch(void *context,
		boost::property_tree::ptree *pt_server,
		boost::property_tree::ptree *pt_client,
		int me,
		int clients_in)
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
    cmd.complete = 0;
    muxq.add_to_runqueue(&cmd);
    while(!cmd.complete);
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
    cmd.complete = 0;
    muxq.add_to_runqueue(&cmd);
    while(!cmd.complete);
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
    mux_state_t *cmd;
    int ok = 0;
    while(true) {
      cmd = muxq.get_from_runqueue();
      if(cmd == NULL) {
	continue;
      }
      if(cmd->op == RING_DOORBELL) {
	cpaths.ring_doorbell(cmd->mc,
			     cmd->client,
			     cmd->port);
      }
      else {
	cyclone_tx(cpaths.socket(cmd->mc, cmd->client),
		   (const unsigned char *)cmd->data,
		   cmd->size,
		   "demux data tx");
      }
      __sync_synchronize();
      cmd->complete = 1;
      __sync_synchronize();
    }
  }
  
  ~server_switch()
  {
    // TBD
   }
};

static int dpdk_server_tx(void * arg)
{
  server_switch *sw = (server_switch *)arg;
  (*sw)();
  return 0;
}

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
      cyclone_bind_endpoint(sockets_in[i], me_mc, q_client, pt_client);
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
#if defined(DPDK_STACK)
    return 0;
#else
    return ports_in[machine];
#endif
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
