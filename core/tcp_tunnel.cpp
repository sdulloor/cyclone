#include "libcyclone.hpp"
#include "tcp_tunnel.hpp"
#include<netinet/in.h>
static tunnel_t *client2server_tunnels;
tunnel_t *server2server_tunnels;
static tunnel_t *server2client_tunnels;

sockaddr_in *server_addresses;
int *sockets_raft;

static sockaddr_in **client_addresses;

tunnel_t* server2server_tunnel(int server, int quorum)
{
  return &server2server_tunnels[server*num_quorums + quorum];
}
 
tunnel_t* server2client_tunnel(int client, int tid)
{
  return &server2client_tunnels[num_clients*tid + client];
}

tunnel_t* client2server_tunnel(int server, int quorum)
{
  return &client2server_tunnels[server*num_quorums + quorum];
}

void server_connect_server(int quorum,
			   int me,
			   int replicas)
{
  struct sockaddr_in serv_addr;
  for(int i=0;i<replicas;i++) {
    if(i == me) {
      continue;
    }
    tunnel_t *tun = server2server_tunnel(i, quorum);
    tun->socket_snd = socket(AF_INET, SOCK_STREAM, 0); 
    if(tun->socket_snd < 0) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to create server 2 server send socket"
	<< " for quorum = " << quorum;
      exit(-1);
    }
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_port   = htons(PORT_SERVER_BASE + quorum*num_queues);
    serv_addr.sin_addr   = server_addresses[i].sin_addr;
    int e = connect(tun->socket_snd, 
		    (struct sockaddr *) &serv_addr,
		    sizeof(serv_addr));
    if (e < 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Unable to connect to replica address "
			       << serv_addr.sin_addr.s_addr
			       <<" port "
			       << serv_addr.sin_port
			       <<" error = "
			       << errno;
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "Quorum = " << quorum
			    <<" connected to replica "
			    << i;
  }
  BOOST_LOG_TRIVIAL(info) << "Quorum = " << quorum
			  <<" connections complete";
}

void client_connect_server(int replicas)
{
  for(int i=0;i<replicas;i++) {
    for(int j=0;j<num_quorums;j++) {
      tunnel_t *tun = client2server_tunnel(i, j);
      // TBD
    }
  }
}

void server_open_ports(int me, int quorum)
{
  struct sockaddr_in iface;
  sockets_raft[quorum] = socket(AF_INET, SOCK_STREAM, 0);
  if(sockets_raft[quorum] < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to get socket for quorum " 
			     << quorum;
    exit(-1);
  }
  iface.sin_family = AF_INET;
  iface.sin_port   = htons(PORT_SERVER_BASE + quorum*num_queues);
  iface.sin_addr = server_addresses[me].sin_addr;
  //iface.sin_addr.s_addr = INADDR_ANY;
  if(bind(sockets_raft[quorum], (struct sockaddr *)&iface, sizeof(iface)) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to bind raft socket "
			     << " quorum =  " << quorum
			     << " address = " << iface.sin_addr.s_addr
			     << " port = " << iface.sin_port;
    exit(-1);
  }
  if(listen(sockets_raft[quorum], 100) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to set listen state for raft socket "
			     << " quorum =  " << quorum;
    exit(-1);
  }
}

void server_accept_server(int socket,
			  int quorum, 
			  int replicas)
{
  struct sockaddr_in sockaddr;
  socklen_t socklen = sizeof(sockaddr_in);
  int sock_rcv;
  tunnel_t *tun;
  for(int i=1;i<replicas;i++) {
    sock_rcv = accept(socket, (struct sockaddr *)&sockaddr, &socklen);
    // Figure out who it is.
    tun = NULL;
    for(int j=0;j<replicas;j++) {
      if(memcmp(&server_addresses[j].sin_addr, 
		&sockaddr.sin_addr, 
		sizeof(struct in_addr)) == 0){
	tun = server2server_tunnel(j, quorum);
	BOOST_LOG_TRIVIAL(info) << "Quorum = "
				<< quorum
				<< " received connect from " 
				<<j;
	break;
      }
    }
    if(tun == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "recvd connect from unknown server: "
			       << sockaddr.sin_addr.s_addr;
      exit(-1);
    }
    if(tun->socket_rcv < 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Server accept server call failed";
      exit(-1);
    }
    tun->socket_rcv = sock_rcv;
  }
}

void server_accept_client(int socket)
{
  for(int i=0;i<num_clients;i++) {
    for(int j=0;j<executor_threads;j++) {
      tunnel_t *tun = server2client_tunnel(i, j);
      tun->socket_rcv = accept(socket, NULL, NULL);
      if(tun->socket_rcv < 0) {
	BOOST_LOG_TRIVIAL(fatal) << "Server accept client call failed";
	exit(-1);
      }
    }
  }
}


void server_connect_client()
{
  for(int i=0;i<num_clients;i++) {
    for(int j=0;j<executor_threads;j++) {
      tunnel_t *tun = server2client_tunnel(i, j);
      // TBD
    }
  }
}
