#ifndef _DISPATCHER_EXEC_
#define _DISPATCHER_EXEC_
#include "cyclone.hpp"
#include "clock.hpp"
#include "runq.hpp"

typedef struct rpc_info_st {
  rpc_t *rpc;
  int raft_idx;
  int raft_term;
  int len;
  volatile int sz;
  void * volatile ret_value;
  void *follower_data;
  int follower_data_size;
  volatile bool have_follower_data;
  volatile bool rep_success;
  volatile bool rep_failed;
  volatile bool rep_follower_success;
  volatile bool complete;
  volatile unsigned long pending_lock;
  volatile int client_blocked;
  unsigned long ticket;
  unsigned char *client_buffer;
  struct rpc_info_st *next;
  struct rpc_info_st *volatile next_issue;
} rpc_info_t;

typedef struct follower_req_st {
  char * req_follower_data;
  int req_follower_data_size;
  int req_follower_term;
  struct follower_req_st * volatile next_issue;
  volatile int done;
} follower_req_t;

typedef struct ticket_st {
  volatile unsigned long go_ticket;
} ticket_t;

extern ticket_t ticket_window;

extern void dispatcher_exec_startup();
extern void exec_rpc(rpc_info_t *rpc);
extern void exec_rpc_internal(rpc_info_t *rpc);
extern void exec_rpc_internal_synchronous(rpc_info_t *rpc);
extern void exec_rpc_internal_ro(rpc_info_t *rpc);
extern void exec_rpc_internal_seq(rpc_info_t *rpc);
extern void exec_send_checkpoint(void *socket, void *handle);

#endif
