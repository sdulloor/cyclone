// Dispatcher test client
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>
#include <libcyclone.hpp>

rtc_clock timer;

static void print(const char *prefix,
		  const void *data,
		  const int size)
{
  unsigned long elapsed_usecs = timer.current_time();
  char *buf = (char *)data;
  if(size != 16) {
    BOOST_LOG_TRIVIAL(fatal) << "CLIENT: Incorrect record size";
    exit(-1);
  }
  else {
    BOOST_LOG_TRIVIAL(info)
      << prefix << " "
      << *(const unsigned int *)buf << " "
      << *(const unsigned int *)(buf + 4) << " " 
      << (elapsed_usecs - *(const unsigned long *)((char *)buf + 8));
  }
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
    *(unsigned long *)&proposal[8] = timer.current_time();
    void *resp;
    print("PROPOSE", proposal, 16);
    int sz = make_rpc(handle, proposal, 16, &resp);
    if(sz != 16 || memcmp(proposal, resp, 16) != 0) {
      print("ERROR", proposal, 16);
    }
    else {
      print("ACCEPTED", proposal, 16);
    }
    ctr++;
  }
}
