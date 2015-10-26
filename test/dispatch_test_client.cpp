// Dispatcher test client
#include "../core/clock.hpp"
#include "../core/cyclone_comm.hpp"
#include<boost/log/trivial.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include <libcyclone.hpp>

rtc_clock timer;

void print(const char *prefix,
	   const void *data,
	   const int size)
{
  unsigned int elapsed_msecs = timer.current_time()/1000;
  char *buf = (char *)data;
  if(size != 12) {
    BOOST_LOG_TRIVIAL(fatal) << "CLIENT: Incorrect record size";
    exit(-1);
  }
  else {
    BOOST_LOG_TRIVIAL(info)
      << prefix << " "
      << *(const unsigned int *)buf << " "
      << *(const unsigned int *)(buf + 4) << " " 
      << (elapsed_msecs - *(const unsigned int *)((char *)buf + 8));
  }
}

int main(int argc, char *argv[])
{
  std::stringstream key;
  std::stringstream addr;
  if(argc != 2) {
    printf("Usage: %s client_id\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  boost::property_tree::ptree pt;
  boost::property_tree::read_ini("cyclone_test.ini", pt);
  void *zmq_context = zmq_init(1);
  int replicas = pt.get<int>("network.replicas");
  void **sockets = new void *[replicas];
  unsigned long port = pt.get<unsigned long>("dispatch.baseport");
  for(int i=0;i<replicas;i++) {
    sockets[i] = dispatch_socket_out(zmq_context);
    key.str("");key.clear();
    addr.str("");addr.clear();
    key << "network.addr" << i;
    addr << "tcp://";
    addr << pt.get<std::string>(key.str().c_str());
    unsigned long endport = port + i;
    addr << ":" << endport;
    cyclone_connect_endpoint(sockets[i], addr.str().c_str());
  }
  int ctr = RPC_INIT_TXID;
  void *buf;
  buf = new char[DISP_MAX_MSGSIZE];
  rpc_t *packet_out = (rpc_t *)buf;
  buf = new char[DISP_MAX_MSGSIZE];
  rpc_t *packet_in = (rpc_t *)buf;
  char *proposal = (char *)(packet_out + 1);
  unsigned long server = 0;
  int retcode;
  void *poll_item = setup_cyclone_inpoll(&sockets[server], 1);
  while(true) {
    *(unsigned int *)&proposal[0] = me;
    *(unsigned int *)&proposal[4] = ctr;
    *(unsigned int *)&proposal[8] = (unsigned int)(timer.current_time()/1000);
    packet_out->code      = RPC_REQ_FN;
    packet_out->client_id = me;
    packet_out->client_txid = ctr;
    retcode = cyclone_tx(sockets[server], (unsigned char *)packet_out, sizeof(rpc_t) + 12, "PROPOSE");
    if(retcode == -1) {
      server = (server + 1)%replicas;
      delete_cyclone_inpoll(poll_item);
      poll_item = setup_cyclone_inpoll(&sockets[server], 1);
      BOOST_LOG_TRIVIAL(info) << "CLIENT DETECTED POSSIBLE FAILED MASTER";
      BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW MASTER " << server;
      continue;
    }
    print("CLIENT PROPOSED", proposal, 12);
    int e = cyclone_poll(poll_item, 1, 10000);
    if(cyclone_socket_has_data(poll_item, 0)) {
      cyclone_rx(sockets[server], (unsigned char *)packet_in, DISP_MAX_MSGSIZE, "RESULT");
    }
    else {
      server = (server + 1)%replicas;
      delete_cyclone_inpoll(poll_item);
      poll_item = setup_cyclone_inpoll(&sockets[server], 1);
      BOOST_LOG_TRIVIAL(info) << "CLIENT DETECTED POSSIBLE FAILED MASTER";
      BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW MASTER " << server;
      continue;
    }
    print("CLIENT REPLY", proposal, 12);
    if(packet_in->code == RPC_REP_INVTXID) {
      ctr = packet_in->client_txid;
      print("CLIENT INVTXID", proposal, 12);
    }
    if(packet_in->code  == RPC_REP_PENDING ||
       packet_in->code  == RPC_REP_INVTXID) {
      // Loop till complete
      while(true) {
	packet_out->code        = RPC_REQ_STATUS;
	packet_out->client_id   = me;
	packet_out->client_txid = ctr;
	int retcode = cyclone_tx(sockets[server], (unsigned char *)packet_out, sizeof(rpc_t) + 12, "PROPOSE");
	if(retcode == -1) {
	  server = (server + 1)%replicas;
	  continue;
	}
	print("CLIENT STATUS", proposal, 12);
	int e = cyclone_poll(poll_item, 1, 10000);
	if(cyclone_socket_has_data(poll_item, 0)) {
	  cyclone_rx(sockets[server], (unsigned char *)packet_in, DISP_MAX_MSGSIZE, "RESULT");
	}
	else {
	  server = (server + 1)%replicas;
	  delete_cyclone_inpoll(poll_item);
	  poll_item = setup_cyclone_inpoll(&sockets[server], 1);
	  BOOST_LOG_TRIVIAL(info) << "CLIENT DETECTED POSSIBLE FAILED MASTER";
	  BOOST_LOG_TRIVIAL(info) << "CLIENT SET NEW MASTER " << server;
	  continue;
	}
	print("CLIENT REPLY", proposal, 12);
	if(packet_in->code == RPC_REP_COMPLETE) {
	  print("CLIENT COMPLETE", proposal, 12);
	  break;
	}
	else if(packet_in->code == RPC_REP_INVSRV) {
	  print("CLIENT INVSRV", proposal, 12);
	  server = packet_in->master;
	  BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING MASTER " << server;
	}
	else if(packet_in->code == RPC_REP_INVTXID) {
	  print("CLIENT INVTXID", proposal, 12);
	  ctr = packet_in->client_txid;
	}
	sleep(1);
      }
      ctr++;
    }
    else if(packet_in->code == RPC_REP_INVSRV) {
      print("CLIENT INVSRV", proposal, 12);
      server = packet_in->master;
      BOOST_LOG_TRIVIAL(info) << "CLIENT SETTING MASTER " << server;
      continue;
    }
  }
}
