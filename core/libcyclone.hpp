#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_
#include<libpmemobj.h>
#ifndef UINT64_MAX
#define UINT64_MAX (-1UL)
#endif
TOID_DECLARE(char, 0);
#define TOID_NUM_BASE 1000
static const int MAX_CLIENTS      = 10000; // Should be enough ?
static const int DISP_MAX_MSGSIZE = 1024; // 1024 bytes max msg size

typedef struct rpc_cookie_st {
  volatile int raft_idx;
  volatile int raft_term;
  volatile int client_id;
  volatile int client_txid;
  void *volatile ret_value;
  volatile int ret_size;
} rpc_cookie_t;

////// RPC Server side interface
// Returns a handle to the transaction
typedef 
void* (*rpc_callback_t)(const unsigned char *data,
			const int len,
			rpc_cookie_t * rpc_cookie);

typedef 
void* (*rpc_leader_callback_t)(const unsigned char *data,
			       const int len,
			       unsigned char **follower_data,
			       int * follower_data_size, 
			       rpc_cookie_t *rpc_cookie);

typedef 
void* (*rpc_follower_callback_t)(const unsigned char *data,
				 const int len,
				 unsigned char *follower_data,
				 int follower_data_size, 
				 rpc_cookie_t * rpc_cookie);



//Garbage collect return value
typedef void (*rpc_gc_callback_t)(void *data);

// Get most recent global cookie data (dont keep lock)
typedef void (*rpc_get_lock_cookie_callback_t)(rpc_cookie_t *cookie);
// Get most recent client specific cookie data (keep lock)
typedef void (*rpc_get_cookie_callback_t)(rpc_cookie_t *cookie);
// Unlock cookie lock
typedef void (*rpc_unlock_cookie_callback_t)();

//NVheap setup return heap root -- passes in recovered heap root
typedef TOID(char) (*rpc_nvheap_setup_callback_t)(TOID(char) recovered,
						  PMEMobjpool *state);
// TX control functions
typedef void (*rpc_tx_commit_callback_t)(void *handle, rpc_cookie_t *cookie);
typedef void (*rpc_tx_abort_callback_t)(void *handle);

// Callback hell !
typedef struct rpc_callbacks_st {
  rpc_callback_t rpc_callback;
  rpc_leader_callback_t rpc_leader_callback;
  rpc_follower_callback_t rpc_follower_callback;
  rpc_get_cookie_callback_t cookie_get_callback;
  rpc_get_lock_cookie_callback_t cookie_lock_callback;
  rpc_unlock_cookie_callback_t cookie_unlock_callback;
  rpc_gc_callback_t gc_callback;
  rpc_nvheap_setup_callback_t nvheap_setup_callback;
  rpc_tx_commit_callback_t tx_commit;
  rpc_tx_abort_callback_t tx_abort;
} rpc_callbacks_t;

// Start the dispatcher loop -- note: does not return
void dispatcher_start(const char* config_server_path,
		      const char* config_client_path,
		      rpc_callbacks_t *rpc_callbacks,
		      int me,
		      int replicas,
		      int clients);

////// RPC client side interface
static const int CLIENT_MAXPAYLOAD = (DISP_MAX_MSGSIZE - 512);
void cyclone_client_global_init(); // NOT THREAD SAFE
void* cyclone_client_init(int client_id,
			  int client_mc,
			  int replicas,
			  const char *config_server,
			  const char *config_client);
void *cyclone_client_dup(void *handle, int me);
// Make an rpc call -- returns size of response
int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response,
	     int txid,
	     int rpc_flags);
// Get last accepred txid
int get_last_txid(void *handle);
// Get the last response
int get_response(void *handle,
		 void **response,
		 int txid);

int delete_node(void *handle, int txid, int node);

int add_node(void *handle, int txid, int node);


// Possible flags 
static const int RPC_FLAG_SYNCHRONOUS   = 1; // Synchronous execution across replicas
static const int RPC_FLAG_RO            = 2; // Read-only RPC
static const int RPC_FLAG_SEQ           = 4;

// Possible error codes
static const int RPC_EOLD               = -1; // RPC too old to cache result
static const int RPC_EUNKNOWN           = -2; // RPC never seen (too new)

#endif
