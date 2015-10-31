#ifndef _DISPATCHER_EXEC_
#define _DISPATCHER_EXEC_
struct rpc_info {
  rpc_t *rpc;
  volatile bool executed;
  volatile bool replicated;
  volatile bool complete;
  struct rpc_info *next;
};
extern rpc_info_t * volatile exec_list;
extern rpc_callback_t execute_rpc;
#endif
