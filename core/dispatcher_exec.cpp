#include "dispatcher_exec.hpp"
rpc_callback_t execute_rpc;
boost::asio::io_service ioService;
boost::asio::io_service::work work(ioService);
boost::thread_group threadpool;
void dispatcher_exec_startup()
{
  threadpool.create_thread
    (boost::bind(&boost::asio::io_service::run, ioService));
}

static void event_committed(const rpc_t *rpc)
{
  int client_id = rpc->client_id;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  if(rpc->client_txid > D_RO(root)->client_state[client_id].committed_txid) {
    D_RW(root)->client_state[client_id].committed_txid = rpc->client_txid;
    unsigned long *ptr =
      (unsigned long  *)&D_RW(root)->client_state[client_id].committed_txid;
    pmemobj_tx_add_range_direct(ptr, sizeof(unsigned long));
    *ptr = rpc->client_txid;
  }
}

// This function must be executed in the context of a tx
static void event_executed(const rpc_t *rpc,
			   const void* ret_value,
			   const int ret_size)
{
  int client_id = rpc->client_id;
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  void *old = (void *)&D_RW(root)->client_state[client_id].last_return_value;
  pmemobj_tx_add_range_direct(old, sizeof(TOID(char)));
  if(!TOID_IS_NULL(D_RW(root)->client_state[client_id].last_return_value))
  {
    TX_FREE(D_RW(root)->client_state[client_id].last_return_value);
  }
  if(ret_size > 0) {
    D_RW(root)->client_state[client_id].last_return_value =
      TX_ALLOC(char, ret_size);
    TX_MEMCPY(D_RW(D_RW(root)->client_state[client_id].last_return_value),
	      ret_value,
	      ret_size);
  }
  else {
    TOID_ASSIGN(D_RW(root)->client_state[rpc->client_id].last_return_value, OID_NULL);
  }
}

static void exec_rpc_internal(rpc_info_t *rpc)
{
  TOID(disp_state_t) root = POBJ_ROOT(state, disp_state_t);
  TX_BEGIN(state) {
    rpc->sz = execute_rpc((const unsigned char *)(rpc->rpc + 1),
			  len - sizeof(rpc_t),
			  &rpc->ret_value);
    rpc->executed = true;
    while(!rpc->rep_success && !rpc->rep_failed);
    if(rpc->rep_success) {
      event_executed(rpc->rpc);
      event_committed(rpc->rpc);
      D_RW(root)->committed_global_txid = rpc->rpc->global_txid;
    }
    else {
      pmemobj_tx_abort(-1);
    }
  } TX_END
  __sync_synchronize();
  rpc->executed = true; // in case the execution aborted (on all replicas !)
  rpc->complete = true; // note: rpc will be freed after this
}

void exec_rpc(rpc_info_t *rpc)
{
  ioService.post(boost::bind(exec_rpc_internal, rpc));
}

