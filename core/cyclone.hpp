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

extern void *cyclone_set_img_build(void *cyclone_handle);
extern void *cyclone_unset_img_build(void *cyclone_handle);

// Callback to build image
typedef void (*cyclone_build_image_t)(void *socket);
					    
// Returns a cyclone handle
extern void* cyclone_boot(const char *config_quorum_path,
			  void *router,
			  int quorum_id,
			  int me,
			  int clients,
			  void *user_arg);

extern void cyclone_shutdown(void *cyclone_handle);

//////// Cfg changes
typedef struct cfg_change_st {
  int node; // Node to be added/deleted
} cfg_change_t;

// Comm between disp core and raft core
typedef struct wal_entry_st {
  volatile int rep;
  int leader;
} __attribute__((packed)) wal_entry_t;

//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int payload_sz;
  int core_id;
  wal_entry_t wal;
  int client_id;
  int requestor;
  int client_port;
  union {
    unsigned long client_txid;
    unsigned long last_client_txid;
  };
  unsigned long channel_seq;
  unsigned long timestamp; // For tracing
} __attribute__((packed)) rpc_t; // Used for both requests and replies


// Possble values for code follow

// Request

static const int RPC_REQ_STATUS         = 0; // Check status 
static const int RPC_REQ_LAST_TXID      = 1; // Get last seen txid from this client
static const int RPC_REQ_FN             = 2; // Execute 
static const int RPC_REQ_NODEADD        = 3; // Add a replica 
static const int RPC_REQ_NODEADDFINAL   = 4; // Completion of add replica
static const int RPC_REQ_NODEDEL        = 5; // Delete a replica 
static const int RPC_REQ_NOOP           = 6; // No-op
static const int RPC_REQ_KICKER         = 7; // Leader change

// Responses
static const int RPC_REP_COMPLETE       = 8; // DONE 
static const int RPC_REP_UNKNOWN        = 9; // UNKNOWN RPC
static const int RPC_REP_INVSRV         = 10; // Not leader
static const int RPC_REP_OLD            = 11; // RPC too old to cache results
#endif
