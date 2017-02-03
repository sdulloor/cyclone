#include "libcyclone.hpp"
#include "tcp_tunnel.hpp"
static tunnel_t *client2server_tunnels;
static tunnel_t *server2server_tunnels;
static tunnel_t *server2client_tunnels;

tunnel_t* server2server_tunnel(int server, int quorum)
{
  return &server2server_tunnels[server*num_quorums + quorum];
}
 
tunnel_t* server2client_tunnel(int client, int tid)
{
  return &server2client_tunnels[num_clients*tid + client];
}

tunnel_t* client2server_tunnel(int server)
{
  return &client2server_tunnels[server];
}

void server_connect_server(int quorum,
			   int replicas, 
			   char *addresses[])
{
  for(int i=0;i<replicas;i++) {
    tunnel_t *tun = server2server_tunnel(i, quorum);
    // TBD
  }
}

void server_accept_server(int quorum, int replicas)
{
  for(int i=0;i<replicas;i++) {
    tunnel_t *tun = server2server_tunnel(i, quorum);
    // TBD
  }

}

void server_accept_client()
{
  for(int i=0;i<num_clients;i++) {
    for(int j=0;j<executor_threads;j++) {
      tunnel_t *tun = server2client_tunnel(i, j);
      // TBD
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
