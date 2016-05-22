#ifndef _COMMON_PMEM_UTILS_
#define _COMMON_PMEM_UTILS_

#include<libcyclone.hpp>

struct client_state_st {
  volatile unsigned long committed_txid;
  TOID(char) last_return_value;
  int last_return_size;
};

typedef struct cookies_st {
  int applied_raft_idx;
  int applied_raft_term;
  struct client_state_st client_state[MAX_CLIENTS];
} cookies_t;

static void lock(volatile unsigned long *lockp)
{
  // TEST + TEST&SET
  do {
    while((*lockp) != 0);
  } while(!__sync_bool_compare_and_swap(lockp, 0, 1));
  __sync_synchronize();
}

static void unlock(volatile unsigned long *lockp)
{
  __sync_synchronize();
  __sync_bool_compare_and_swap(lockp, 1, 0);
  __sync_synchronize();
}

extern void get_cookie(rpc_cookie_t *cookie);
extern void get_lock_cookie(rpc_cookie_t *cookie);
extern void unlock_cookie();
extern void mark_done(rpc_cookie_t *cookie);
#endif
