#ifndef _CYCLONE_COMM_ZMQ_
#define _CYCLONE_COMM_ZMQ_

// Best effort
static int cyclone_tx(void *socket,
		      const unsigned char *data,
		      unsigned long size,
		      const char *context) 
{
  int rc = zmq_send(socket, data, size, ZMQ_DONTWAIT);
  if(rc == -1) {
    if (errno != EAGAIN) {
      BOOST_LOG_TRIVIAL(fatal) 
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

// Keep trying until success
static void cyclone_tx_block(void *socket,
			     const unsigned char *data,
			     unsigned long size,
			     const char *context)
{
  int ok;
  do {
    ok = cyclone_tx(socket, data, size, context);
  } while(ok != 0);
}

// Block till data available
static int cyclone_rx_block(void *socket,
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
  return rc;
}

// Block till data available or timeout
static int cyclone_rx_timeout(void *socket,
			      unsigned char *data,
			      unsigned long size,
			      unsigned long timeout_usecs,
			      const char *context)
{
  int rc;
  unsigned long mark = rtc_clock::current_time();
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
    if((rtc_clock::current_time() - mark) >= timeout_usecs) {
      break;
    }
  }
  return rc;
}

// Best effort
static int cyclone_rx(void *socket,
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
  return rc;
}

static void* cyclone_socket_out(void *context)
{
  void *socket;
  int conflate = 1;
  socket = zmq_socket(context, ZMQ_PUSH);
  int e = zmq_setsockopt(socket, ZMQ_CONFLATE, &conflate, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock CONFLATE "
      << context << " "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }

  int linger = 0;
  e = zmq_setsockopt(socket, ZMQ_LINGER, &linger, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock linger "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
  
  return socket;
}


static void* cyclone_socket_in(void *context)
{
  void *socket;
  int recv_hwm = 50;
  socket = zmq_socket(context, ZMQ_PULL);
  int e = zmq_setsockopt(socket, ZMQ_RCVHWM, &recv_hwm, sizeof(int));
  if (e == -1) {
    BOOST_LOG_TRIVIAL(fatal) 
      << "CYCLONE_COMM: Unable to set sock RCVHWM "
      << context << " "
      << zmq_strerror(zmq_errno());
    exit(-1);
  }
  return socket;
}


static void cyclone_connect_endpoint(void *socket,
				     int mc,
				     int port,
				     boost::property_tree::ptree *pt)
{

  std::stringstream key; 
  std::stringstream addr;
  key.str("");key.clear();
  addr.str("");addr.clear();
  key << "machines.addr" << i;
  addr << "tcp://";
  addr << pt->get<std::string>(key.str().c_str());
  addr << ":" << port;
  BOOST_LOG_TRIVIAL(info)
    << "CYCLONE::COMM Connecting to "
    << endpoint;
  zmq_connect(socket, endpoint, addr.str().c_str());
}

static void cyclone_bind_endpoint(void *socket,
				  int mc,
				  int port,
				  boost::property_tree::ptree *pt)
{
  std::stringstream key; 
  std::stringstream addr;
  key.str("");key.clear();
  addr.str("");addr.clear();
  key << "machines.iface" << me;
  addr << "tcp://";
  addr << pt->get<std::string>(key.str().c_str());
  if(port != -1) 
    addr << ":" << port;
  else
    addr << ":*";
  int rc = zmq_bind(socket, addr.str().c_str());
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




#endif
