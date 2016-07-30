#ifndef _COMMON_PMEM_UTILS_
#define _COMMON_PMEM_UTILS_

#include<libcyclone.hpp>

struct client_state_st {
  volatile unsigned long committed_txid;
  int size;
};

struct client_state_indirect_st {
  PMEMoid state; 
} __attribute__((aligned(64)));

typedef struct cookies_st {
  struct client_state_indirect_st client_state[MAX_CLIENTS];
} cookies_t;

const int CSTATE_TYPE_NUM = TOID_NUM_BASE - 1;

extern void begin_tx();
extern void commit_tx(void *, rpc_cookie_t *);
extern void abort_tx(void *);
extern void init_cstate(PMEMobjpool *pop, PMEMoid *cs);
extern void init_cookie_system(PMEMobjpool *pool, cookies_t *root);
extern void get_cookie(rpc_cookie_t *cookie);
extern void get_lock_cookie(rpc_cookie_t *cookie);
extern void unlock_cookie();
#endif
