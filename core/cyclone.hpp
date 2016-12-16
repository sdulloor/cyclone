#ifndef _CYCLONE_
#define _CYCLONE_
extern "C" {
 #include<raft.h>
}
#include "libcyclone.hpp"
#include <stdlib.h>
#include <string.h>

//////// Direct interface
int cyclone_is_leader(void *cyclone_handle); // returns 1 if true
int cyclone_get_leader(void *cyclone_handle); // returns leader id
int cyclone_get_term(void *cyclone_handle); // Get current term
void *cyclone_control_socket_out(void *cyclone_handle, 
				 int replica); // Get control out socket
void *cyclone_control_socket_in(void *cyclone_handle); // Get control in socket

int cyclone_serialize_last_applied(void *cyclone_handle, void *buf);

extern void *cyclone_set_img_build(void *cyclone_handle);
extern void *cyclone_unset_img_build(void *cyclone_handle);

// Callback to build image
typedef void (*cyclone_build_image_t)(void *socket);
					    
// Returns a cyclone handle
extern void* cyclone_setup(const char *config_quorum_path,
			   void *router,
			   int quorum_id,
			   int me,
			   int clients,
			   void *user_arg);
extern void cyclone_boot();

extern void cyclone_shutdown(void *cyclone_handle);

//////// Cfg changes
typedef struct cfg_change_st {
  int node; // Node to be added/deleted
} cfg_change_t;

// Comm between app core and raft core
typedef struct wal_entry_st {
  volatile int rep;
  int term;
  int idx;
  int leader;
} __attribute__((packed)) wal_entry_t;

//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int payload_sz;
  unsigned long core_mask;
  wal_entry_t wal;
  int client_id;
  int requestor;
  int client_port;
  unsigned long channel_seq;
  unsigned long timestamp; // For tracing
} __attribute__((packed)) rpc_t; // Used for both requests and replies

//////// Addendum for inter-core rendevouz
typedef struct ic_rdv_st{
  char mc_id[6];
  unsigned long rtc_ts;
} __attribute__((packed)) ic_rdv_t; 

static ic_rdv_t *rpc2rdv(rpc_t *rpc)
{
  unsigned char *ptr = (unsigned char *)rpc;
  ptr = ptr + num_quorums*sizeof(unsigned long);
  return (ic_rdv_t *)ptr;
}

typedef struct core_status_st {
  volatile int exec_term;
  volatile int checkpoint_idx;
  volatile int stable;
  ic_rdv_t nonce;
  volatile int success;
  volatile unsigned int barrier[2];
} __attribute__((aligned(64))) core_status_t;

extern core_status_t *core_status;

static unsigned long check_terms(unsigned int *snapshot, unsigned long mask)
{
  unsigned long failed = 0;
  while(mask != 0) {
    int core = __builtin_ffsl(mask) - 1;
    if(snapshot[core] != core_status[core].exec_term) {
      failed = failed | (1UL << core);
    }
    mask = mask & ~(1UL << core);
  }
  return failed;
}


static int wait_barrier_follower(core_status_t *c, 
				 ic_rdv_t *nonce,
				 int core_id,
				 unsigned int *snapshot,
				 unsigned long mask)
{
  ic_rdv_t *n = NULL;
  int stable;
  int cnt = __builtin_popcountl(mask);
  int success;
  while(true) {
    if(!check_terms(snapshot, 1UL << (__builtin_ffsl(mask) - 1))) {
      return 0;
    }
    stable = c->stable;
    if(stable & 1)
      continue;
    if(memcmp(&c->nonce, nonce, sizeof(ic_rdv_t)) != 0)
      continue;
    if(stable != c->stable)
      continue;
  }
  __sync_fetch_and_or(&c->barrier[0], 1UL << core_id);
  while(c->barrier[0] != mask);
  success = c->success;
  __sync_fetch_and_or(&c->barrier[1], 1UL << core_id);
  return success;
}


static int wait_barrier_leader(core_status_t *c,
			       ic_rdv_t *nonce,
			       int core_id,
			       unsigned int *snapshot,
			       unsigned long mask)
{
  ic_rdv_t *n = NULL;
  int stable;
  int cnt = __builtin_popcountl(mask);
  c->stable++;
  __sync_synchronize();
  memcpy(&c->nonce, nonce, sizeof(ic_rdv_t));
  __sync_synchronize();
  c->stable++;
  __sync_fetch_and_or(&c->barrier[0], 1UL << core_id);
  unsigned long failed_mask = 0;
  while(c->barrier[0] != mask) {
    failed_mask = failed_mask | check_terms(snapshot, mask);
    if(failed_mask) {
      __sync_fetch_and_or(&c->barrier[0], failed_mask);
    }
  }
  c->success = (failed_mask ? 0:1);
  __sync_fetch_and_or(&c->barrier[1], 1UL << core_id);
  __sync_fetch_and_or(&c->barrier[1], failed_mask);
  while(c->barrier[1] != mask);
  c->success = 0;
  c->barrier[0] = 0;
  c->barrier[1] = 0;
  if(failed_mask) {
    return 0;
  }
  else {
    return 1;
  }
}

// Possble values for code
static const int RPC_REQ_STABLE         = 0; // Check for stable quorums
static const int RPC_REQ                = 1; // RPC request 
static const int RPC_REQ_KICKER         = 2; // RPC internal 
static const int RPC_REQ_NODEADDFINAL   = 3; // RPC internal 
static const int RPC_REQ_NODEADD        = 4; // Add node 
static const int RPC_REQ_NODEDEL        = 5; // Delete node 
static const int RPC_REP_OK             = 6; // RPC response OK
static const int RPC_REP_FAIL           = 7; // RPC response FAILED 

#endif
