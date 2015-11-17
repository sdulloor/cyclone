#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <boost/log/trivial.hpp>
#include <zmq.h>
#include <unistd.h>
#include "clock.hpp"

// Cyclone communication

static int cyclone_tx(void *socket,
		      const unsigned char *data,
		      unsigned long size,
		      const char *context) 
{
  int rc = zmq_send(socket, data, size, ZMQ_NOBLOCK);
  if(rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(warning) 
	<< "CYCLONE: Unable to transmit "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
    return -1;
  }
  else {
    return 0;
  }
}

static int cyclone_tx_timeout(void *socket,
			      const unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs,
			      const char *context) 
{
  rtc_clock clock;
  int rc;
  clock.start();
  while (true) {
    rc = zmq_send(socket, data, size, ZMQ_NOBLOCK);
    if(rc >= 0) {
      break;
    }
    if(rc == -1) {
      if (errno != EAGAIN) {
	BOOST_LOG_TRIVIAL(warning) 
	  << "CYCLONE: Unable to transmit "
	  << context << " "
	  << zmq_strerror(zmq_errno());
	exit(-1);
      }
    }
    clock.stop();
    if(clock.elapsed_time() >= timeout_usecs) {
      break;
    }
    clock.start();
  }
  return rc;
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
	BOOST_LOG_TRIVIAL(fatal) 
	  << "CYCLONE: Unable to receive "
	  << context << " "
	  << zmq_strerror(zmq_errno());
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

static unsigned long cyclone_rx_timeout(void *socket,
					unsigned char *data,
					unsigned long size,
					unsigned long timeout_usecs,
					const char *context)
{
  int rc;
  rtc_clock clock;
  clock.start();
  while (true) {
    rc = zmq_recv(socket, data, size, ZMQ_NOBLOCK);
    if(rc >= 0) {
      break;
    }
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE: Unable to receive "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
    clock.stop();
    if(clock.elapsed_time() >= timeout_usecs) {
      break;
    }
    clock.start();
  }
  return (unsigned long) rc;
}

static unsigned long cyclone_rx_noblock(void *socket,
					unsigned char *data,
					unsigned long size,
					const char *context)
{
  int rc;
  rc = zmq_recv(socket, data, size, ZMQ_NOBLOCK);
  if (rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE: Unable to receive "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
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

static void delete_cyclone_inpoll(void *pollitem)
{
  delete[] ((zmq_pollitem_t *)pollitem);
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

static void socket_set_nolinger(void *socket)
{
  int linger = 0;
  int e = zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock linger "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
}

static void* cyclone_socket_out(void *context, bool use_hwm)
{
  void *socket;
  int hwm = 5;
  socket = zmq_socket(context, ZMQ_PUSH);
  if(use_hwm) {
    int e = zmq_setsockopt(socket, ZMQ_SNDHWM, &hwm, sizeof(int));
    if (e == -1) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "CYCLONE_COMM: Unable to set sock HWM "
	<< context << " "
	<< zmq_strerror(zmq_errno());
      exit(-1);
    }
  }
  socket_set_nolinger(socket);
  return socket;
}

static void* cyclone_socket_out_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REQ);
  socket_set_nolinger(socket);
  return socket;
}

static void* cyclone_socket_in(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_PULL);
  return socket;
}

static void* cyclone_socket_in_loopback(void *context)
{
  void *socket;
  socket = zmq_socket(context, ZMQ_REP);
  return socket;
}

static void cyclone_connect_endpoint(void *socket, const char *endpoint)
{
  BOOST_LOG_TRIVIAL(info)
    << "CYCLONE::COMM Connecting to "
    << endpoint;
  zmq_connect(socket, endpoint);
}

static void cyclone_bind_endpoint(void *socket, const char *endpoint)
{
  int rc = zmq_bind(socket, endpoint);
  if (rc != 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "CYCLONE::COMM Unable to setup listening socket at "
      << endpoint << " "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
  else {
    BOOST_LOG_TRIVIAL(info)
      << "CYCLONE::COMM Listening at "
      << endpoint;
  }
}

class cyclone_switch {
  int machines;
  void **sockets_out;
  void **sockets_in;
  std::stringstream key;
  std::stringstream addr;
public:
  cyclone_switch(void *context, 
		 boost::property_tree::ptree *pt,
		 int me,
		 int machines_in, 
		 int baseport_local,
		 int baseport_remote,
		 bool loopback,
		 bool use_hwm)
    :machines(machines_in)
  {
    sockets_in = new void *[machines];
    sockets_out = new void *[machines];
    unsigned long port;
    for(int i=0;i<machines;i++) {
      // input wire from i
      if(i == me && loopback) {
	sockets_in[i] = cyclone_socket_in_loopback(context);
      }
      else {
	sockets_in[i] = cyclone_socket_in(context);
      }
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "network.iface" << i;
      addr << "tcp://";
      addr << pt->get<std::string>(key.str().c_str());
      port = baseport_local + me*machines + i;
      addr << ":" << port;
      cyclone_bind_endpoint(sockets_in[i], addr.str().c_str());
      // output wire to i
      if(i == me && loopback) {
	sockets_out[i] = cyclone_socket_out_loopback(context);
      }
      else {
	sockets_out[i] = cyclone_socket_out(context, use_hwm);
      }
      key.str("");key.clear();
      addr.str("");addr.clear();
      key << "network.addr" << i;
      addr << "tcp://";
      addr << pt->get<std::string>(key.str().c_str());
      port = baseport_remote + i*machines + me ;
      addr << ":" << port;
      cyclone_connect_endpoint(sockets_out[i], addr.str().c_str());
    }
    
  }

  void * output_socket(int machine)
  {
    return sockets_out[machine];
  }

  void *input_socket(int machine)
  {
    return sockets_in[machine];
  }

  void **input_socket_array()
  {
    return sockets_in;
  }

  ~cyclone_switch()
  {
    for(int i=0;i<=machines;i++) {
      zmq_close(sockets_out[i]);
      zmq_close(sockets_in[i]);
    }
    delete[] sockets_out;
    delete[] sockets_in;
  }
};

#endif
