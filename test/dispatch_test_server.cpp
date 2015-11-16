// Test dispatch/RPC interface (server side)
#include<libcyclone.hpp>
#include<string.h>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include<stdio.h>

unsigned long server_id;

//Print message and reflect back the rpc payload
int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  void *ret = malloc(len);
  memcpy(ret, data, len);
  *return_value = ret;
  return len;
}

TOID(char) nvheap_setup(TOID(char) recovered, PMEMobjpool *state)
{
  return TOID_NULL(char);
}

void gc(void *data)
{
  free(data);
}

int main(int argc, char *argv[])
{
  if(argc != 2) {
    printf("Usage: %s server_id\n", argv[0]);
    exit(-1);
  }
  server_id = atoi(argv[1]);
  dispatcher_start("cyclone_test.ini", callback, gc, nvheap_setup, server_id);
}
