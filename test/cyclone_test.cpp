// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<libcyclone.hpp>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>

rtc_clock timer;

void print(const char *prefix,
	   void *data,
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

void cyclone_cb(void *user_arg, const unsigned char *data, const int len)
{
  print("CLIENT: APPLY", (void *)data, len);
}

void cyclone_rep_cb(void *user_arg, const unsigned char *data, const int len)
{
  print("CLIENT: REP", (void *)data, len);
}

void cyclone_pop_cb(void *user_arg, const unsigned char *data, const int len)
{
  print("CLIENT: POP", (void *)data, len);
}

int main(int argc, char *argv[])
{
  void *cyclone_handle;
  if(argc != 2) {
    printf("Usage %s node_id\n", argv[0]);
    exit(-1);
  }
  timer.start();
  unsigned int node_id = atoi(argv[1]);
  unsigned char entry[12];
  int ctr = 0;
  cyclone_handle = cyclone_boot("cyclone_test.ini",
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_cb,
				NULL);
  while(true) {
    if(cyclone_is_leader(cyclone_handle) == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) = ctr;
    *(unsigned int *)(entry + 8) =
      (unsigned int)(timer.current_time()/1000);
    void *cookie;
    do {
      usleep(30000);
      cookie = cyclone_add_entry(cyclone_handle, entry, 12);
    } while(cookie == NULL);
    int result;
    do {
      usleep(30000);
      result = cyclone_check_status(cyclone_handle, cookie);
    } while(result == 0);
    free(cookie);
    unsigned int elapsed_msecs = timer.current_time()/1000;
    if(result == 1) {
      print("CLIENT: ACCEPT ", (void *)entry, 12);
    }
    else {
      print("CLIENT: REJECT ", (void *)entry, 12);
    }
    ctr++;
  }
}
