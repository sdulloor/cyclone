// Basic test driver for cyclone
#include<stdio.h>
#include<libcylone.hpp>

void cyclone_cb(ocnst unsigned char *data, const int len)
{
  if(len != 8) {
    fprintf(stderr, "ERROR\n");
  }
  else {
    fprintf(stderr, "LOG %d:%d\n",
	    *(const unsigned int *)data,
	    *(const unsigned int *)(data + 4));
  }
}

int main(int argc, void *argv)
{
  if(argc != 2) {
    printf("Usage %s node_id\n", argv[0]);
    exit(-1);
  }
  unsigned int node_id = atoi(argv[1]);
  unsigned char entry[8];
  unsigned int ctr = 1;
  cyclone_req_t req;
  cyclone_boot("cyclone_test.ini", &cyclone_cb);
  
  while(true) {
    if(cyclone_is_leader() == 0)
      continue;
    *(unsigned int *)entry = node_id;
    *(unsigned int *)(entry + 4) = ctr;
    req.data = entry;
    req.size = 8;
    req.request_complete = 0;
    if(cyclone_add_entry(&req) != 0) {
      continue;
    }
    ctr++;
  }
}
