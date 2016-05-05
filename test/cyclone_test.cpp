// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include "../core/cyclone.hpp"
#include "../core/clock.hpp"
#include "../core/logging.hpp"
#include "../core/tuning.hpp"

void print(const char *prefix,
	   void *data,
	   const int size)
{
  unsigned long elapsed_usecs = rtc_clock::current_time();
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

void cyclone_rep_cb(void *user_arg,
		    const unsigned char *data,
		    const int len,
		    const int raft_idx,
		    const int raft_term)
{
  print("CLIENT: REP", (void *)data, len);
}

void cyclone_pop_cb(void *user_arg,
		    const unsigned char *data,
		    const int len,
		    const int raft_idx,
		    const int raft_term)
{
  print("CLIENT: POP", (void *)data, len);
}

int main(int argc, char *argv[])
{
  void *cyclone_handle;
  if(argc != 3) {
    printf("Usage %s node_id replicas\n", argv[0]);
    exit(-1);
  }
  unsigned int node_id = atoi(argv[1]);
  unsigned int replicas = atoi(argv[2]);
  unsigned char entry[16];
  int ctr = 0;
  cyclone_handle = cyclone_boot("cyclone_test.ini",
				&cyclone_rep_cb,
				&cyclone_pop_cb,
				&cyclone_cb,
				NULL,
				node_id,
				replicas,
				NULL);
  while(true) {
    if(cyclone_is_leader(cyclone_handle) == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) = ctr;
    void *cookie;
    do {
      *(unsigned long *)(entry + 8) = rtc_clock::current_time();
      cookie = cyclone_add_entry(cyclone_handle, entry, 16);
      if(cookie == NULL) {
	BOOST_LOG_TRIVIAL(info) << "CLIENT: Not on master";
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
    unsigned long elapsed_usecs = rtc_clock::current_time();
    if(result == 1) {
      print("CLIENT: ACCEPT ", (void *)entry, 16);
    }
    else {
      print("CLIENT: REJECT ", (void *)entry, 16);
    }
    ctr++;
  }
}
