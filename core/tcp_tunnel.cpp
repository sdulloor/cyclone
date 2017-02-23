#include "libcyclone.hpp"
#include "tcp_tunnel.hpp"
#include<sys/types.h>
#include<sys/socket.h>
#include<netinet/in.h>
#include<netinet/tcp.h>

tunnel_t *server2server_tunnels;
tunnel_t **server2client_tunnels;

sockaddr_in *server_addresses;
int *sockets_raft;
int *sockets_client;

sockaddr_in *client_addresses;

tunnel_t* server2server_tunnel(int server, int quorum)
{
  return &server2server_tunnels[server*num_quorums + quorum];
}
 
tunnel_t* server2client_tunnel(int client, int quorum)
{
  return server2client_tunnels[quorum*num_clients + client];
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

void client_connect_server(int clientnum,
			   int replica, 
			   int quorum, 
			   tunnel_t *tun)
{
  struct sockaddr_in serv_addr;
  tun->socket_snd = socket(AF_INET, SOCK_STREAM, 0); 
  if(tun->socket_snd < 0) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "Unable to create server 2 server send socket"
      << " for quorum = " << quorum;
    exit(-1);
  }
  serv_addr.sin_family = AF_INET;
  serv_addr.sin_port   = htons(PORT_SERVER_BASE + num_quorums*num_queues + quorum);
  serv_addr.sin_addr   = server_addresses[replica].sin_addr;
  int e = connect(tun->socket_snd, 
		  (struct sockaddr *) &serv_addr,
		  sizeof(serv_addr));
  if (e < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Client unable to connect to replica address "
			     << serv_addr.sin_addr.s_addr
			     <<" port "
			     << serv_addr.sin_port
			     <<" error = "
			     << errno;
    exit(-1);
  }
  int flag = 1;
  int result = setsockopt(tun->socket_snd,            /* socket affected */
			  IPPROTO_TCP,     /* set option at TCP level */
			  TCP_NODELAY,     /* name of option */
			  (char *) &flag,  /* the cast is historical cruft */
			  sizeof(int));    /* length of option value */
  if(result < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to set nodelay ...";
    exit(-1);
  }


  tun->socket_rcv = tun->socket_snd;
  tun->send_buf((void *)&clientnum, sizeof(int));
  BOOST_LOG_TRIVIAL(info) << "Client "
			  << " connected to replica "
			  << replica
			  << " quorum = "
			  << quorum;
}

void server_open_ports(int me, int quorum)
{
  struct sockaddr_in iface;
  
  // RAFT port
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

  // Client port
  sockets_client[quorum] = socket(AF_INET, SOCK_STREAM, 0);
  if(sockets_client[quorum] < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to get socket for client " 
			     << quorum;
    exit(-1);
  }
  iface.sin_family = AF_INET;
  iface.sin_port   = htons(PORT_SERVER_BASE + num_quorums*num_queues + quorum);
  iface.sin_addr = server_addresses[me].sin_addr;
  //iface.sin_addr.s_addr = INADDR_ANY;
  if(bind(sockets_client[quorum], (struct sockaddr *)&iface, sizeof(iface)) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to bind client socket "
			     << " quorum =  " << quorum
			     << " address = " << iface.sin_addr.s_addr
			     << " port = " << iface.sin_port;
    exit(-1);
  }
  if(listen(sockets_client[quorum], 100) < 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to set listen state for client socket "
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
    if(sock_rcv < 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Server accept server call failed";
      exit(-1);
    }
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
    tun->socket_rcv = sock_rcv;
  }
}

void server_accept_client(int socket, int quorum)
{
  struct sockaddr_in sockaddr;
  socklen_t socklen = sizeof(sockaddr_in);
  int sock_rcv;
  char buf[10];
  for(int i=0;i<num_clients;i++) {
    tunnel_t *tun = (tunnel_t *)malloc(sizeof(tunnel_t));
    tun->init();
    sock_rcv = accept(socket, (struct sockaddr *)&sockaddr, &socklen);
    if(sock_rcv < 0) {
      BOOST_LOG_TRIVIAL(fatal) << "Server accept server call failed";
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "Quorum = "
			    << quorum
			    << " received connect from client"; 
    tun->socket_rcv = sock_rcv;
    tun->socket_snd = sock_rcv;
    // Figure out who it is.
    while(!tun->receive());
    tun->copy_out_buf(buf);
    int client = *(int *)buf;
    BOOST_LOG_TRIVIAL(info) << "quorum = "
			    << quorum
			    <<" connect from client "
			    << client;
    server2client_tunnels[quorum*num_clients + client] = tun;
  }
  BOOST_LOG_TRIVIAL(info) << "Client accept complete.";
}


