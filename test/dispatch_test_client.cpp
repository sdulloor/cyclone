// Dispatcher test client
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>
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

void trace_send_cmd(void *data, const int size)
{
  print("SERVER: SEND_CMD", data, size);
}

void trace_recv_cmd(void *data, const int size)
{
  print("SERVER: RECV_CMD", data, size);
}

void trace_pre_append(void *data, const int size)
{
  print("SERVER: PRE_APPEND", data, size);
}

void trace_post_append(void *data, const int size)
{
  print("SERVER: POST_APPEND", data, size); 
}

void trace_send_entry(void *data, const int size)
{
  print("SERVER: SEND_ENTRY", data, size);
}

void trace_recv_entry(void *data, const int size)
{
  print("SERVER: RECV_ENTRY", data, size);
}

int main(int argc, char *argv[])
{
  boost::log::keywords::auto_flush = true;
  if(argc != 2) {
    printf("Usage: %s client_id\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  void * handle = cyclone_client_init(me, "cyclone_test.ini");
  char *proposal = new char[CLIENT_MAXPAYLOAD];
  int ctr = 0;
  while(true) {
    *(unsigned int *)&proposal[0] = me;
    *(unsigned int *)&proposal[4] = ctr;
    *(unsigned int *)&proposal[8] = (unsigned int)(timer.current_time()/1000);
    void *resp;
    print("PROPOSE", proposal, 12);
    int sz = make_rpc(handle, proposal, 12, &resp);
    if(sz != 12 || memcmp(proposal, resp, 12) != 0) {
      print("ERROR", proposal, 12);
    }
    else {
      print("ACCEPTED", proposal, 12);
    }
    ctr++;
  }
}
