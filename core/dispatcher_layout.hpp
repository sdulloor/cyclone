#ifndef _DISPATCHER_LAYOUT_
#define _DISPATCHER_LAYOUT_
#include<libpmemobj.h>
#include "libcyclone.hpp"
struct client_state_st {
  volatile unsigned long committed_txid;
  TOID(char) last_return_value;
  int last_return_size;
};
POBJ_LAYOUT_BEGIN(disp_state);
typedef struct disp_state_st {
  TOID(char) nvheap_root;
  volatile int applied_raft_idx;
  volatile int applied_raft_term;
  struct client_state_st client_state[MAX_CLIENTS];
} disp_state_t;
TOID_DECLARE_ROOT(disp_state_t);
POBJ_LAYOUT_END(disp_state);
#endif
