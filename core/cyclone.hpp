#ifndef _CYCLONE_
#define _CYCLONE_
#include<libpmemobj.h>
#include "libcyclone.hpp"

//////// Direct interface
int cyclone_is_leader(void *cyclone_handle); // returns 1 if true
int cyclone_get_leader(void *cyclone_handle); // returns leader id
// Returns a non-null cookie if accepted for replication
extern void *cyclone_add_entry(void * cyclone_handle, void *data, int size); 
// Returns 0:pending 1:success -1:failed
extern int cyclone_check_status(void *cyclone_handle, void *cookie);
// Callback to apply a log entry
typedef void (*cyclone_callback_t)(void *user_arg,
				   const unsigned char *data,
				   const int len);
// Returns a cyclone handle
extern void* cyclone_boot(const char *config_path,
			  cyclone_callback_t cyclone_rep_callback,
			  cyclone_callback_t cyclone_pop_callback,
			  cyclone_callback_t cyclone_commit_callback,
			  int me,
			  int replicas,
			  void *user_arg);
extern void cyclone_shutdown(void *cyclone_handle);

//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int client_id;
  unsigned long global_txid;
  unsigned long timestamp;
  union {
    unsigned long client_txid; 
    int master;
  };
  unsigned char payload[0];
} rpc_t; // Used for both requests and replies

// Possble values for code follow
static const int RPC_REQ_FN             = 0; // Execute
static const int RPC_REQ_STATUS         = 1; // Check status (non blocking)
static const int RPC_REQ_STATUS_BLOCK   = 2; // Check status (blocking)
static const int RPC_REP_COMPLETE       = 3; // DONE 
static const int RPC_REP_PENDING        = 4; // PENDING 
static const int RPC_REP_REDO           = 5; // Redo RPC
static const int RPC_REP_INVSRV         = 6; // WRONG master  -- master set in reply

static const unsigned long RPC_INIT_TXID = 1; // Initial client txid

#endif
