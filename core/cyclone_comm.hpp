#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include "cyclone_context.hpp"
// Cyclone communication

static void cyclone_tx(void *socket,
		       unsigned char *data,
		       unsigned long size,
		       const char *context) {
  while (true) {
    int rc = zmq_send(socket, data, size, 0);
    if (rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(warning) << "CYCLONE: Unable to transmit";
	perror(context);
	//Communcation failures are acceptable
      }
      // Retry
    }
    else {
      break;
    }
  }
}

static unsigned long cyclone_rx(void *socket,
				unsigned char *data,
				unsigned long size,
				const char *context)
{
  int rc;
  while (true) {
    rc = zmq_recv(socket, data, size, 0);
    if (rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(fatal) << "CYCLONE: Unable to receive";
	perror(context);
	exit(-1);
      }
      // Retry
    }
    else {
      break;
    }
  }
  return (unsigned long) rc;
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

#endif
