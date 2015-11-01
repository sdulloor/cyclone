#ifndef _DISPATCHER_EXEC_
#define _DISPATCHER_EXEC_
#include "libcyclone.hpp"
typedef struct rpc_info_st {
  rpc_t *rpc;
  int len;
  int sz;
  void *ret_value;
  volatile bool executed;
  volatile bool rep_success;
  volatile bool rep_failed;
  volatile bool complete;
  struct rpc_info_st *next;
} rpc_info_t;
extern void exec_rpc(rpc_info_t *rpc);
extern rpc_callback_t execute_rpc;
extern PMEMobjpool *state;
#endif
