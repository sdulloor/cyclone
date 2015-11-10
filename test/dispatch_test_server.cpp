// Test dispatch/RPC interface (server side)
#include<libcyclone.hpp>
#include<string.h>
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>

unsigned long server_id;

rtc_clock timer;

void print(const char *prefix,
	   const void *data,
	   const int size)
{
  unsigned long elapsed_usecs = timer.current_time();
  char *buf = (char *)data;
  if(size != 16 && size != (16 + sizeof(rpc_t))) {
    BOOST_LOG_TRIVIAL(fatal) << "SERVER: Incorrect record size"
			     << size;
    exit(-1);
  }
  if(size > 16) {
    buf = buf + sizeof(rpc_t);
  }
    
  BOOST_LOG_TRIVIAL(info)
    << prefix << " "
    << *(const unsigned int *)buf << " "
    << *(const unsigned int *)(buf + 4) << " " 
    << (elapsed_usecs - *(const unsigned long *)((char *)buf + 8));
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

//Print message and reflect back the rpc payload
int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  print("SERVER: APPLY", data, len);
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
  server_id = dispatcher_me();
  dispatcher_start("cyclone_test.ini", callback, gc, nvheap_setup);
}
