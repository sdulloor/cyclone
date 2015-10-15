// Dispatcher for cyclone
#include<stdio.h>
#include<stdlib.h>
#include<string.h>
#include<errno.h>
#include<unistd.h>
#include "libcyclone.hpp"
#include "../core/clock.hpp"
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include<boost/log/trivial.hpp>
#include<libpmemobj.h>

static void *cyclone_handle;
static boost::property_tree::ptree pt;

POBJ_LAYOUT_BEGIN(disp_state);
typedef struct disp_state_st {
  unsigned long committed_client_txid[MAX_CLIENTS];
} disp_state_t;
POBJ_LAYOUT_ROOT(disp_state, disp_state_t);
POBJ_LAYOUT_END(disp_state);
static PMEMobjpool *state;


unsigned long seen_client_txid[MAX_CLIENTS];
static void (*execute_rpc)(const unsigned char *data, const int len);

void cyclone_commit_cb(void *user_arg, const unsigned char *data, const int len)
{
  // Call back to app.
  const rpc_t *rpc = (const rpc_t *)data;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  if(rpc->client_txid >
     D_RO(root)->committed_client_txid[rpc->client_id]) {
    TX_BEGIN(state) {
      void *ptr = (void *)&D_RO(root)->committed_client_txid[rpc->client_id];
      pmemobj_tx_add_range_direct(ptr, sizeof(unsigned long));
      D_RW(root)->committed_client_txid[rpc->client_id] = rpc->client_txid;
    } TX_END
  }
  if(rpc->client_txid > seen_client_txid[rpc->client_id]) {
    seen_client_txid[rpc->client_id] = rpc->client_txid;    
  }
  execute_rpc(data, len);
}

void cyclone_rep_cb(void *user_arg, const unsigned char *data, const int len)
{
  // Call back to app.
  const rpc_t *rpc = (const rpc_t *)data;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  if(rpc->client_txid > seen_client_txid[rpc->client_id]) {
    seen_client_txid[rpc->client_id] = rpc->client_txid;    
  }
}

void dispatcher_start(const char* config_path,
		      void (*rpc_callback)(const unsigned char *data, const int len))
{
  boost::property_tree::read_ini(config_path, pt);
  cyclone_handle = cyclone_boot(config_path,
				&cyclone_rep_cb,
				&cyclone_commit_cb,
				NULL);
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
   seen_client_txid[i] = D_RO(root)->committed_client_txid[i];
  }
  execute_rpc = rpc_callback;
  // Listen on port
  
}
