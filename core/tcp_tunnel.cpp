#include "libcyclone.hpp"
#include "tcp_tunnel.hpp"
static tunnel_t *client2server_tunnels;
static tunnel_t *server2server_tunnels;
static tunnel_t *server2client_tunnels;

tunnel_t* server2server_tunnel(int server, 
			       int quorum,
			       int queue)
{
  return &server2server_tunnels[server*num_quorums*num_queues + quorum*num_queues];
}
 
tunnel_t* server2client_tunnel(int client,
			       int tid)
{
  return &server2client_tunnels[num_clients*tid + client];
}

tunnel_t* client2server_tunnel(int server)
{
  return &client2server_tunnels[server];
}




