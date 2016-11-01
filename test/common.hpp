#ifndef _COMMON_PMEM_UTILS_
#define _COMMON_PMEM_UTILS_

#include<libcyclone.hpp>
#include "../core/tuning.hpp"

struct client_state_st {
  volatile unsigned long committed_txid;
  int size;
};

struct client_state_indirect_st {
  PMEMoid state; 
};

typedef struct cookies_st {
  struct client_state_indirect_st client_state[executor_threads][MAX_CLIENTS];
} cookies_t;

const int CSTATE_TYPE_NUM = TOID_NUM_BASE - 1;

extern void begin_tx();
extern void commit_tx(rpc_cookie_t *);
extern void abort_tx();
extern void init_cstate(PMEMobjpool *pop, PMEMoid *cs);
extern void init_cookie_system(PMEMobjpool *pool, cookies_t *root);
extern void get_cookie(rpc_cookie_t *cookie);



static void spin_lock(volatile unsigned long *lockp)
{
  // TEST + TEST&SET
  do {
    while((*lockp) != 0);
  } while(!__sync_bool_compare_and_swap(lockp, 0, 1));
  __sync_synchronize();
}

static void spin_unlock(volatile unsigned long *lockp)
{
  __sync_synchronize();
  __sync_bool_compare_and_swap(lockp, 1, 0);
  __sync_synchronize();
}




#endif
