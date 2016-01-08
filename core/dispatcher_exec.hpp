#ifndef _DISPATCHER_EXEC_
#define _DISPATCHER_EXEC_
#include "cyclone.hpp"
#include "clock.hpp"

typedef struct rpc_info_st {
  rpc_t *rpc;
  int len;
  int sz;
  void *ret_value;
  unsigned long follower_data_lock;
  void *follower_data;
  int follower_data_size;
  volatile bool replicate_to_followers;
  volatile int replicate_to_follower_term;
  volatile bool rep_success;
  volatile bool rep_failed;
  volatile bool complete;
  struct rpc_info_st *next;
} rpc_info_t;


extern void dispatcher_exec_startup();
extern void exec_rpc(rpc_info_t *rpc);
extern void exec_rpc_internal(rpc_info_t *rpc);
extern void exec_rpc_internal_synchronous(rpc_info_t *rpc);
extern void exec_rpc_internal_ro(rpc_info_t *rpc);
#endif
