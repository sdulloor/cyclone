#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_
#include<libpmemobj.h>

TOID_DECLARE(char, 0);
#define TOID_NUM_BASE 1000
static const int MAX_CLIENTS      = 10000; // Should be enough ?
static const int DISP_MAX_MSGSIZE = 4194304; // 4MB max msg size 

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
		      rpc_nvheap_setup_callback_t nvheap_setup_callback,
		      int me,
		      int replicas,
		      int clients);

////// RPC client side interface
static const int CLIENT_MAXPAYLOAD = (DISP_MAX_MSGSIZE - 512);
void* cyclone_client_init(int client_id, int replicas, int clients, const char *config);
// Make an rpc call -- returns size of response
int make_rpc(void *handle,
	     void *payload,
	     int sz,
	     void **response);


#endif
