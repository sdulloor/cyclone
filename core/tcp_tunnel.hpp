// Setup a TCP/IP tunnel for dpdk
#ifndef TCP_TUNNEL
#define TCP_TUNNEL
#include "logging.hpp"
#include <rte_mbuf.h>
#include <rte_ether.h>
#include <rte_ip.h>
#include <sys/types.h>
#include <sys/socket.h>


// Settings
const int MSG_MAX = 4096;

typedef struct tunnel_st {
  int msg_sz;
  char *msg;
  char *fragment;
  int socket_snd;
  int socket_rcv;

  int ready()
  {
    if(msg_sz > 0 && *(int *)msg == msg_sz) {
      return 1;
    }
    else {
       return 0;
    }
  }

  void init()
  {
    msg = (char *)malloc(MSG_MAX);
    if(msg == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Unable to allocate msg buffer" ;
      exit(-1);
    }
    fragment = (char *)malloc(MSG_MAX);
    if(fragment == NULL) {
      BOOST_LOG_TRIVIAL(fatal) << "Unable to allocate fragment buffer" ;
      exit(-1);
    }
    msg_sz = 0;
  }
  
  int receive()
  {
    if(ready()) {
      return 1;
    }
    int bytes = recv(socket_rcv, 
		     fragment, 
		     MSG_MAX,
		     MSG_DONTWAIT);
    if(bytes > 0) {
      memcpy(msg + msg_sz, fragment, bytes);
      msg_sz += bytes;
    }
    return ready();
  }
  
  // Note: must only call when ready
  void copy_out(rte_mbuf *pkt)
  {
    int bytes = msg_sz - sizeof(int);
    memcpy(rte_pktmbuf_mtod_offset(pkt, 
				   void *,
				   sizeof(struct ether_hdr) +
				   sizeof(struct ipv4_hdr)),
	   msg + sizeof(int), 
	   bytes);
    
    pkt->pkt_len = 
      sizeof(struct ether_hdr) + 
      sizeof(struct ipv4_hdr)  + 
      bytes;
    pkt->data_len = pkt->pkt_len;
   msg_sz = 0;
  }

  void send(rte_mbuf *pkt)
  {
    int bytes = sizeof(int);
    while(pkt) {
      memcpy(fragment + bytes, 
	     rte_pktmbuf_mtod(pkt, void*),
	     pkt->data_len);
      bytes += pkt->data_len;
      pkt    = pkt->next;
    }

    *(int *)fragment = bytes;
    char * buf =fragment;
    while(bytes) {
      int bytes_sent = ::send(socket_snd, buf, bytes, 0);
      if(bytes_sent > 0) {
	bytes -= bytes_sent;
	buf   += bytes_sent;
      }
    }
  }
}tunnel_t;

extern tunnel_t *server2server_tunnels;
extern tunnel_t* server2server_tunnel(int server, int quorum);
extern tunnel_t* server2client_tunnel(int client, int tid); 
extern tunnel_t* client2server_tunnel(int server);
extern void server_open_ports(int me, int quorum);
extern void server_accept_server(int socket,
				 int quorum, 
				 int replicas);

extern void server_connect_server(int quorum,
				  int me,
				  int replicas);
extern sockaddr_in *server_addresses;
extern int *sockets_raft;
// Random fixed port numbers
const int PORT_SERVER_BASE = 0xf16;

#endif
