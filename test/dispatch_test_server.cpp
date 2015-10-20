// Test dispatch/RPC interface (server side)
#include<libcyclone.hpp>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>

unsigned long server_id;

rtc_clock timer;

void print(const char *prefix,
	   const void *data,
	   const int size)
{
  unsigned int elapsed_msecs = timer.current_time()/1000;
  char *buf = (char *)data;
  if(size != 12) {
    BOOST_LOG_TRIVIAL(fatal) << "SERVER: Incorrect record size";
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

int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  print("SERVER: APPLY", data, len);
  return 0;
}

int main(int argc, char *argv[])
{
  server_id = dispatcher_me();
  dispatcher_start("dispatcher.ini", callback);
}
