// Setup a TCP/IP tunnel for dpdk
#ifndef TCP_TUNNEL
#define TCP_TUNNEL
#include "logging.hpp"

// Settings
const int MSG_MAX = 4096;
const int CLIENTS = 100;


typedef struct tunnel_st {
  int msg_sz;
  char *msg;
  char *fragment;
  int socket;

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
  
  int recv()
  {
    if(ready()) {
      return 1;
    }
    int bytes = recv(socket, 
		     fragment, 
		     MSG_MAX,
		     MSG_DONTWAIT);
    if(bytes > 0) {
      memcpy(msg + msg_sz, fragment, fragment_sz);
      msg_sz += fragment_sz;
      fragment_sz = 0;
    }
    return ready();
  }
  
  int copy_out(void *buffer)
  {
    if(!ready())
      return 0;
    int bytes = msg_sz - sizeof(int);
    memcpy(buffer, msg + sizeof(int), bytes);
    msg_sz = 0;
    return bytes;
  }

  void send(void *buffer, int sz)
  {
    int bytes  = sz + sizeof(int);
    *(int *)fragment = sz;
    memcpy(fragment + sizeof(int), buffer, sz);
    char * buf =fragment;
    while(bytes) {
      int bytes_sent = send(socket, buf, bytes, 0);
      if(bytes_sent > 0) {
	bytes -= bytes_sent;
      }
    }
  }
}tunnel_t;


int client_connect(int client_main_socket)
{
  sockaddr sock_addr;
  socklen_t socket_len;
  int id;
  int e = accept(client_main_socket,
		 &sock_addr,
		 &socket_len);
  if(e >= 0) {
    return e;
  }
  else {
    return 0;
  }
}

#endif
