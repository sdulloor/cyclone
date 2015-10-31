#ifndef _DISPATCHER_EXEC_
#define _DISPATCHER_EXEC_
#include "libcyclone.hpp"
typedef struct rpc_info_st {
  rpc_t *rpc;
  volatile bool executed;
  volatile bool replicated;
  volatile bool complete;
  struct rpc_info_st *next;
} rpc_info_t;
extern rpc_info_t * volatile exec_list;
extern rpc_callback_t execute_rpc;
#endif
