#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_
#include<libpmemobj.h>


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
			  void *user_arg);
extern void cyclone_shutdown(void *cyclone_handle);

//////// RPC interface
TOID_DECLARE(char, 0);
#define TOID_NUM_BASE 1000
static const int MAX_CLIENTS      = 10000; // Should be enough ?
static const int DISP_MAX_MSGSIZE = 4194304; // 4MB max msg size 

typedef struct rpc_st {
  int code;
  int client_id;
  unsigned long global_txid;
  union {
    unsigned long client_txid; 
    int master;
  };
  unsigned char payload[0];
} rpc_t; // Used for both requests and replies

// Possble values for code follow
static const int RPC_REQ_FN       = 0; // Execute
static const int RPC_REQ_STATUS   = 1; // Check status
static const int RPC_REP_COMPLETE = 2; // DONE 
static const int RPC_REP_PENDING  = 3; // PENDING 
static const int RPC_REP_INVTXID  = 4; // WRONG client txid -- last seen
				       // client_txid set in reply
static const int RPC_REP_INVSRV   = 5; // WRONG master  -- master set in reply

static const unsigned long RPC_INIT_TXID = 1; // Initial client txid

////// RPC Server side interface
// Returns the size of the return value blob
typedef 
int (*rpc_callback_t)(const unsigned char *data,
		      const int len,
		      void **return_value);

//Garbage collect return value
typedef void (*rpc_gc_callback_t)(void *data);

//NVheap setup return heap root -- passes in recovered heap root
typedef TOID(char) (*rpc_nvheap_setup_callback_t)(TOID(char) recovered,
						  PMEMobjpool *state);

// Start the dispatcher loop -- note: does not return
void dispatcher_start(const char* config_path, 
		      rpc_callback_t rpc_callback,
		      rpc_gc_callback_t gc_callback,
		      rpc_nvheap_setup_callback_t nvheap_setup_callback);

// My id
int dispatcher_me();


////// RPC client side interface
static const int CLIENT_MAXPAYLOAD = (DISP_MAX_MSGSIZE - sizeof(rpc_t));
void* cyclone_client_init(int client_id, const char *config);
// Make an rpc call -- returns size of response
int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response);


#endif
