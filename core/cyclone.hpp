#ifndef _CYCLONE_
#define _CYCLONE_
#include<libpmemobj.h>
extern "C" {
 #include<raft.h>
}
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
extern void *cyclone_add_batch(void * cyclone_handle, void *data, int* sizes, int batch_size); 
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
// Callback to add, remove or commit a log entry
typedef void (*cyclone_callback_t)(void *user_arg,
				   const unsigned char *data,
				   const int len,
				   const int raft_idx,
				   const int raft_term);

typedef void (*cyclone_rep_callback_t)(void *user_arg,
				       const unsigned char *data,
				       const int len,
				       const int raft_idx,
				       const int raft_term,
				       replicant_t *rep);
// Callback to build image
typedef void (*cyclone_build_image_t)(void *socket);
					    
// Returns a cyclone handle
extern void* cyclone_boot(const char *config_path,
			  const char *client_path,
			  cyclone_rep_callback_t cyclone_rep_callback,
			  cyclone_callback_t cyclone_pop_callback,
			  cyclone_callback_t cyclone_commit_callback,
			  cyclone_build_image_t cyclone_build_image_callback,
			  int me,
			  int replicas,
			  int clients,
			  void *user_arg);

extern void cyclone_shutdown(void *cyclone_handle);

//////// Cfg changes
typedef struct cfg_change_st {
  int node; // Client fills in
  int last_included_term; // server fills in
  int last_included_idx; // server fills in
} cfg_change_t;

const int REP_UNKNOWN = 0;
const int REP_SUCCESS = 1;
const int REP_FAILED  = -1;

// Comm between disp cores and raft core
typedef struct wal_entry_st {
  volatile int raft_term;
  volatile int raft_idx;
  int leader;
  volatile int rep;
} wal_entry_t;

//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int requestor;
  int client_id;
  int client_port;
  unsigned long client_txid;
  unsigned long channel_seq;
  union {
    int parent_raft_idx;
    replicant_t rep;
    int master;
    int last_client_txid;
    int parent_raft_term;
    int receiver;
    unsigned long timestamp;
  };
  wal_entry_t wal;
  int payload_sz;
} rpc_t; // Used for both requests and replies


// Possble values for code follow

// Request



static const int RPC_REQ_STATUS         = 0; // Check status (block on completion)
static const int RPC_REQ_LAST_TXID      = 1; // Get last seen txid from this client
static const int RPC_REQ_FN             = 2; // Execute (block on completion)
static const int RPC_REQ_MARKER         = 3; // Dispatcher internal (do not use)
static const int RPC_REQ_DATA           = 4; // Dispatcher internal (do not use)
static const int RPC_REQ_NODEADD        = 5; // Add a replica (non blocking)
static const int RPC_REQ_NODEDEL        = 6; // Delete a replica (non blocking)
static const int RPC_REQ_ASSIST         = 7; // Assist in replication
static const int RPC_REP_ASSIST         = 8; // Assistance response
static const int RPC_REQ_NOOP           = 9; // No-op
// Responses
static const int RPC_REP_ASSIST_OK      = 9; // Assistance entry accepted
static const int RPC_REP_COMPLETE       = 10; // DONE 
static const int RPC_REP_PENDING        = 11; // PENDING 
static const int RPC_REP_UNKNOWN        = 12; // UNKNOWN RPC
static const int RPC_REP_INVSRV         = 13; // WRONG master  -- master set in reply
static const int RPC_REP_OLD            = 14; // RPC too old to cache results
#endif
