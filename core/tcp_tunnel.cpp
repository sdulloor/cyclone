#include "libcyclone.hpp"
#include "tcp_tunnel.hpp"
static tunnel_t *client_tunnels;
static tunnel_t *server_tunnels;

tunnel_t* server_endp2tunnel(int server, 
			     int quorum,
			     int queue)
{
  return &server_tunnels[server*num_quorums*num_queues + quorum*num_queues];
}
 
tunnel_t* client_endp2tunnel(int client)
{
  return &client_tunnels[client];
}

