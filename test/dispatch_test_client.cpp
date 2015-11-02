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

int main(int argc, char *argv[])
{
  std::stringstream key;
  std::stringstream addr;
  boost::log::keywords::auto_flush = true;
  if(argc != 2) {
    printf("Usage: %s client_id\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  void * handle = cyclone_client_init(me, "cyclone_test.ini");
  char *proposal = new char[CLIENT_MAXPAYLOAD];
  unsigned long server = 0;
  int retcode;
  int ctr = 0;
  while(true) {
    *(unsigned int *)&proposal[0] = me;
    *(unsigned int *)&proposal[4] = ctr;
    *(unsigned int *)&proposal[8] = (unsigned int)(timer.current_time()/1000);
    void *resp;
    print("PROPOSE", proposal, 12);
    make_rpc(handle, proposal, 12, &resp);
    print("ACCEPTED", proposal, 12);
    ctr++;
  }
}
