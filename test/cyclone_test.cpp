// Basic test driver for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include<libcyclone.hpp>

void cyclone_cb(const unsigned char *data, const int len)
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
  if(argc != 2) {
    printf("Usage %s node_id\n", argv[0]);
    exit(-1);
  }
  unsigned int node_id = atoi(argv[1]);
  unsigned char entry[8];
  unsigned int ctr = 1;
  cyclone_boot("cyclone_test.ini", &cyclone_cb);
  
  while(true) {
    if(cyclone_is_leader() == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) = ctr;

    void *cookie;
    do {
      sleep(1);
      cookie = cyclone_add_entry(entry, 8);
    } while(cookie == NULL);
    int result;
    do {
      sleep(1);
      result = cyclone_check_status(cookie);
    } while(result == 0);
    
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
