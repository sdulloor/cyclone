#ifndef _CYCLONE_
#define _CYCLONE_
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

// Comm between app core and raft core
typedef struct wal_entry_st {
  volatile int rep;
  int term;
  int idx;
  int leader;
} __attribute__((packed)) wal_entry_t;

typedef struct core_status_st {
  volatile int exec_term;
  volatile int checkpoint_idx;
} __attribute__((aligned(64))) core_status_t;


//////// RPC interface
typedef struct rpc_st {
  int code;
  int flags;
  int payload_sz;
  int core_mask;
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
