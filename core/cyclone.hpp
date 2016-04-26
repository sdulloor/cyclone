#ifndef _CYCLONE_
#define _CYCLONE_
#include<libpmemobj.h>
#include "libcyclone.hpp"

//////// Direct interface
int cyclone_is_leader(void *cyclone_handle); // returns 1 if true
int cyclone_get_leader(void *cyclone_handle); // returns leader id
int cyclone_get_term(void *cyclone_handle); // Get current term
void *cyclone_control_socket_out(void *cyclone_handle, 
				 int replica); // Get control out socket
void *cyclone_control_socket_in(void *cyclone_handle); // Get control in socket

int cyclone_serialize_last_applied(void *cyclone_handle, void *buf);

// Returns a non-null cookie if accepted for replication
extern void *cyclone_add_entry(void * cyclone_handle, void *data, int size); 
extern void *cyclone_add_entry_cfg(void * cyclone_handle,
				   int type,
				   void *data,
				   int size); 
extern void *cyclone_add_entry_term(void * cyclone_handle, 
				    void *data, 
				    int size,
				    int term);
extern void *cyclone_set_img_build(void *cyclone_handle);
extern void *cyclone_unset_img_build(void *cyclone_handle);

// Returns 0:pending 1:success -1:failed
extern int cyclone_check_status(void *cyclone_handle, void *cookie);
// Callback to add or remove a log entry
typedef void (*cyclone_callback_t)(void *user_arg,
				   const unsigned char *data,
				   const int len,
				   const int raft_idx,
				   const int raft_term);
//Callback to commit a log entry
typedef void (*cyclone_commit_t)(void *user_arg,
				 const unsigned char *data,
				 const int len);

// Callback to build image
typedef void (*cyclone_build_image_t)(void *socket);
					    
// Returns a cyclone handle
extern void* cyclone_boot(const char *config_path,
			  cyclone_callback_t cyclone_rep_callback,
			  cyclone_callback_t cyclone_pop_callback,
			  cyclone_commit_t cyclone_commit_callback,
			  cyclone_build_image_t cyclone_build_image_callback,
			  int me,
			  int replicas,
			  void *user_arg);

extern void cyclone_shutdown(void *cyclone_handle);

//////// Cfg changes
typedef struct cfg_change_st {
  int node; // Client fills in
  int last_included_term; // server fills in
  int last_included_idx; // server fills in
} cfg_change_t;

//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int client_id;
  union {
    unsigned long client_txid;
    int parent_raft_idx;
  };
  union {
    int master;
    int last_client_txid;
    int parent_raft_term;
  };
  unsigned long timestamp;
  unsigned long channel_seq;
  int requestor;
  unsigned char payload[0];
} rpc_t; // Used for both requests and replies

// Possble values for code follow
static const int RPC_REQ_FN             = 0; // Execute
static const int RPC_REQ_STATUS         = 1; // Check status (non blocking)
static const int RPC_REQ_STATUS_BLOCK   = 2; // Check status (blocking)
static const int RPC_REQ_LAST_TXID      = 3; // Get last seen txid from this client
static const int RPC_REQ_MARKER         = 4; // Dispatcher internal (do not use)
static const int RPC_REQ_DATA           = 5; // Dispatcher internal (do not use)
static const int RPC_REQ_NODEADD        = 6; // Add a replica
static const int RPC_REQ_NODEDEL        = 7; // Delete a replica
static const int RPC_REP_COMPLETE       = 8; // DONE 
static const int RPC_REP_PENDING        = 9; // PENDING 
static const int RPC_REP_UNKNOWN        = 10; // UNKNOWN RPC
static const int RPC_REP_INVSRV         = 11; // WRONG master  -- master set in reply
static const int RPC_REP_OLD            = 12; // RPC too old to cache results
#endif
