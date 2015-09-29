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

struct cyclone_monitor {
  volatile bool terminate;
  cyclone_t *cyclone_handle;
  cyclone_monitor()
  :terminate(false)
  {}
  
  void operator ()()
  {
    rtc_clock timer;
    zmq_pollitem_t *items = new zmq_pollitem_t[cyclone_handle->replicas];
    for(int i=0;i<cyclone_handle->replicas;i++) {
      items[i].socket = cyclone_handle->zmq_pull_sockets[i];
      items[i].events = ZMQ_POLLIN;
    }
    while(!terminate) {
      timer.start();
      int e = zmq_poll(items, cyclone_handle->replicas, (unsigned long)PERIODICITY_MSEC);
      timer.stop();
      // Handle any outstanding requests
      for(int i=0;i<=cyclone_handle->replicas;i++) {
	if(items[i].revents & ZMQ_POLLIN) {
	  unsigned long sz = do_zmq_recv(cyclone_handle->zmq_pull_sockets[i],
					 cyclone_handle->cyclone_buffer_in,
					 MSG_MAXSIZE,
					 "Incoming");
	  cyclone_handle->handle_incoming(sz);
	}
      }
      int elapsed_time = timer.elapsed_time()/1000;
      // Handle periodic events -- - AFTER any incoming requests
      if(elapsed_time >= PERIODICITY_MSEC) {
	raft_periodic(cyclone_handle->raft_handle, elapsed_time);
	timer.reset();
      }
    }
  }
};

#endif
