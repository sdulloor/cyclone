// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<unistd.h>
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include<libpmemobj.h>

static void *cyclone_handle;
static boost::property_tree::ptree pt;
const int MAX_CLIENTS = 10000; // Should be enough ?

struct rpc_params_st {
  unsigned long server_txid; // Assigned by server
  unsigned long client_txid; // Assigned by server
  unsigned long client_id;
  unsigned char payload[0];
} rpc_params_t;

POBJ_LAYOUT_BEGIN(disp_state);
typedef struct disp_state_st {
  unsigned long committed_server_txid;
  unsigned long committed_client_txid[MAX_CLIENTS];
} disp_state_t;
POBJ_LAYOUT_ROOT(disp_state, disp_state_t);
POBJ_LAYOUT_END(disp_state);
static PMEMobjpool *state;


unsigned long next_server_txid;
unsigned long next_client_txid[MAX_CLIENTS];
static void (*execute_rpc)(const unsigned char *data, const int len);

void cyclone_cb(void *user_arg, const unsigned char *data, const int len)
{
  // Call back to app.
  rpc_params_t *rpc_params = (rpc_params_t *)data;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  if(rpc_params->server_txid > D_RO(root)->committed_server_txid) {
    TX_BEGIN(state) {
      TX_ADD(D_RW(root)->committed_server_txid);
      D_RW(root)->committed_server_txid = rpc_params->server_txid;
    } TX_END
  }
  if(rpc_params->client_txid >
     D_RO(root)->committed_client_txid[rpc->client_id]) {
    TX_BEGIN(state) {
      TX_ADD(D_RW(root)->committed_client_txid[rpc->client_id]);
      D_RW(root)->committed_client_txid[rpc->client_id] = rpc_params->client_txid;
    } TX_END
  }
  execute_rpc(data, len);
}

void dispatcher_start(const char* config_path,
		      void (*rpc_callback)(const char *data, const int len))
{
  boost::property_tree::read_ini(config_path, pt);
  cyclone_handle = cyclone_boot(config_path, &cyclone_cb, NULL);
  // Load/Setup state
  std::string file_path = pt.get<std::string>("dispatch.filepath");
  if(access(file_path.c_str(), F_OK)) {
    state = pmemobj_create(file_path.c_str(),
			   POBJ_LAYOUT_NAME(disp_state),
			   sizeof(disp_state_t) + PMEMOBJ_MIN_POOL,
			   0666);
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to creat pmemobj pool for dispatcher:"
	<< strerror(errno);
      exit(-1);
    }
  
    TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
    TX_BEGIN(state) {
      TX_ADD(root);
      D_RW(root)->committed_server_txid = 0UL;
      for(int i = 0;i < MAX_CLIENTS;i++) {
	D_RW(root)->committed_client_txid[i] = 0UL;
      }
    } TX_ONABORT {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Unable to setup dispatcher state:"
	<< strerror(errno);
      exit(-1);
    } TX_END
  }
  else {
    state = pmemobj_open(file_path.c_str(),
			 "dispatcher_persistent_state");
    if(state == NULL) {
      BOOST_LOG_TRIVIAL(fatal)
	<< "Unable to open pmemobj pool for dispatcher state:"
	<< strerror(errno);
      exit(-1);
    }
    BOOST_LOG_TRIVIAL(info) << "DISPATCHER: Recovered state";
  }
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  for(int i=0;i<MAX_CLIENTS;i++) {
    next_client_txid[i] = D_RO(root)->committed_client_txid[i] + 1;
  }
  next_server_txid = D_RO(root)->committed_server_txid + 1;
  execute_rpc = rpc_callback;
  // Listen on port
  
}
