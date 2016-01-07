#ifndef _CYCLONE_
#define _CYCLONE_
#include<libpmemobj.h>
#include "libcyclone.hpp"

//////// Direct interface
int cyclone_is_leader(void *cyclone_handle); // returns 1 if true
int cyclone_get_leader(void *cyclone_handle); // returns leader id
int cyclone_get_term(void *cyclone_handle); // Get current term
// Returns a non-null cookie if accepted for replication
extern void *cyclone_add_entry(void * cyclone_handle, void *data, int size); 
extern void *cyclone_add_entry_term(void * cyclone_handle, 
				    void *data, 
				    int size,
				    int term);
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
  union {
    unsigned long global_txid;
    int last_client_txid;
  };
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
static const int RPC_REQ_LAST_TXID      = 3; // Get last seen txid from this client
static const int RPC_REQ_MARKER         = 4; // Internal (do not use)
static const int RPC_REP_COMPLETE       = 5; // DONE 
static const int RPC_REP_PENDING        = 6; // PENDING 
static const int RPC_REP_UNKNOWN        = 7; // UNKNOWN RPC
static const int RPC_REP_INVSRV         = 8; // WRONG master  -- master set in reply

#endif
