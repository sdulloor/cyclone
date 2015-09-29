// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<libcyclone.hpp>


void cyclone_cb(void *user_arg, const unsigned char *data, const int len)
{
  if(len != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    fprintf(stderr, "APPLY %d:%d\n",
	    *(const unsigned int *)data,
	    *(const unsigned int *)(data + 4));
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
  unsigned int node_id = atoi(argv[1]);
  unsigned char entry[8];
  unsigned int ctr = 1;
  cyclone_handle = cyclone_boot("cyclone_test.ini", &cyclone_cb, NULL);
  while(true) {
    if(cyclone_is_leader(cyclone_handle) == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) = ctr;

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
    if(result == 1) {
      fprintf(stderr, "LOG %d:%d\n",
	      *(const unsigned int *)entry,
	      *(const unsigned int *)(entry + 4));
    }
    else {
      fprintf(stderr, "REJECT %d:%d\n",
	      *(const unsigned int *)entry,
	      *(const unsigned int *)(entry + 4));
    }
    ctr++;
  }
}
