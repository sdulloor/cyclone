// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<libcyclone.hpp>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include "../core/timeouts.hpp"

rtc_clock timer;

void print(const char *prefix,
	   void *data,
	   const int size)
{
  unsigned long elapsed_usecs = timer.current_time();
  char *buf = (char *)data;
  if(size != 16) {
    BOOST_LOG_TRIVIAL(fatal) << "SERVER: Incorrect record size";
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
  unsigned char entry[16];
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
    *(unsigned long *)(entry + 8) = timer.current_time();
    void *cookie;
    do {
      cookie = cyclone_add_entry(cyclone_handle, entry, 16);
      if(cookie == NULL) {
	usleep(timeout_msec*1000);
      }
      else {
	break;
      }
    } while(true);
    int result;
    do {
      result = cyclone_check_status(cyclone_handle, cookie);
      if(result != 0) {
	break;
      }
    } while(true);
    free(cookie);
    unsigned long elapsed_usecs = timer.current_time();
    if(result == 1) {
      print("CLIENT: ACCEPT ", (void *)entry, 16);
    }
    else {
      print("CLIENT: REJECT ", (void *)entry, 16);
    }
    ctr++;
  }
}
