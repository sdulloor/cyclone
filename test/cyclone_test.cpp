// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<libcyclone.hpp>
#include "../core/clock.hpp"

rtc_clock timer;

void trace_pre_append(void *data, const int size)
{
  if(size != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    unsigned int elapsed_msecs = timer.current_time()/1000;
    fprintf(stderr, "PRE_APPEND %d:%d\n",
	    *(const unsigned int *)data,
	    elapsed_msecs - *(const unsigned int *)((char *)data + 4));
  }
  __sync_synchronize();
}



void trace_post_append(void *data, const int size)
{
  if(size != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    unsigned int elapsed_msecs = timer.current_time()/1000;
    fprintf(stderr, "POST_APPEND %d:%d\n",
	    *(const unsigned int *)data,
	    elapsed_msecs - *(const unsigned int *)((char *)data + 4));
  }
  __sync_synchronize();
}

void trace_send_entry(void *data, const int size)
{
  if(size != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    unsigned int elapsed_msecs = timer.current_time()/1000;
    fprintf(stderr, "SEND_ENTRY %d:%d\n",
	    *(const unsigned int *)data,
	    elapsed_msecs - *(const unsigned int *)((char *)data + 4));
  }
  __sync_synchronize();
}

void trace_recv_entry(void *data, const int size)
{
  if(size != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    unsigned int elapsed_msecs = timer.current_time()/1000;
    fprintf(stderr, "RECV_ENTRY %d:%d\n",
	    *(const unsigned int *)data,
	    elapsed_msecs - *(const unsigned int *)((char *)data + 4));
  }
  __sync_synchronize();
}

void cyclone_cb(void *user_arg, const unsigned char *data, const int len)
{
  if(len != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    unsigned int elapsed_msecs = timer.current_time()/1000;
    fprintf(stderr, "APPLY %d:%d\n",
	    *(const unsigned int *)data,
	    elapsed_msecs - *(const unsigned int *)(data + 4));
  }
  __sync_synchronize();
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
  unsigned char entry[8];
  cyclone_handle = cyclone_boot("cyclone_test.ini", &cyclone_cb, NULL);
  while(true) {
    if(cyclone_is_leader(cyclone_handle) == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) =
      (unsigned int)(timer.current_time()/1000);
    void *cookie;
    do {
      usleep(30000);
      cookie = cyclone_add_entry(cyclone_handle, entry, 8);
    } while(cookie == NULL);
    int result;
    do {
      usleep(30000);
      result = cyclone_check_status(cyclone_handle, cookie);
    } while(result == 0);
    free(cookie);
    unsigned int elapsed_msecs = timer.current_time()/1000;
    if(result == 1) {
      fprintf(stderr, "LOG %d:%d\n",
	      *(const unsigned int *)entry,
	      elapsed_msecs - *(const unsigned int *)(entry + 4));
    }
    else {
      fprintf(stderr, "REJECT %d:%d\n",
	      *(const unsigned int *)entry,
	      elapsed_msecs - *(const unsigned int *)(entry + 4));
    }
  }
}
