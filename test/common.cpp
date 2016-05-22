#include "common.hpp"
#include<libcyclone.hpp>
#include "../core/logging.hpp"

volatile unsigned long cookies_lock = 0;
cookies_t* cookies_root;
PMEMobjpool *cookies_pool;

// This function must be executed in the context of a tx
void mark_done(rpc_cookie_t *cookie)
{
  int client_id = cookie->client_id;
  lock(&cookies_lock);
  volatile int *c_raft_idx_p  = &cookies_root->applied_raft_idx;
  volatile int *c_raft_term_p = &cookies_root->applied_raft_term;
  pmemobj_tx_add_range_direct((const void *)c_raft_idx_p, sizeof(int));
  pmemobj_tx_add_range_direct((const void *)c_raft_term_p, sizeof(int));
  *c_raft_idx_p  = cookie->raft_idx;
  *c_raft_term_p = cookie->raft_term;
  struct client_state_st *cstate = &cookies_root->client_state[cookie->client_id];
  pmemobj_tx_add_range_direct(cstate, sizeof(struct client_state_st));
  if(!TOID_IS_NULL(cstate->last_return_value)) {
    TX_FREE(cstate->last_return_value);
  }
  if(cookie->ret_size > 0) {
    cstate->last_return_value = TX_ALLOC(char, cookie->ret_size);
    if(TOID_IS_NULL(cstate->last_return_value)) {
      BOOST_LOG_TRIVIAL(fatal) << "mark_done: Out of pmem heap space.";
      exit(-1);
    }
    pmemobj_memcpy_persist(cookies_pool, 
			   D_RW(cstate->last_return_value), 
			   cookie->ret_value, 
			   cookie->ret_size);
  }
  else {
    TOID_ASSIGN(cstate->last_return_value, OID_NULL);
  }
  cstate->last_return_size = cookie->ret_size;
  cstate->committed_txid = cookie->client_txid;
  unlock(&cookies_lock);
}


void get_cookie(rpc_cookie_t *cookie)
{
  lock(&cookies_lock);
  cookie->raft_idx  = cookies_root->applied_raft_idx;
  cookie->raft_term = cookies_root->applied_raft_term; 
  unlock(&cookies_lock);
}

void get_lock_cookie(rpc_cookie_t *cookie)
{
  lock(&cookies_lock);
  cookie->raft_idx  = cookies_root->applied_raft_idx;
  cookie->raft_term = cookies_root->applied_raft_term; 
  struct client_state_st *cstate = &cookies_root->client_state[cookie->client_id];
  cookie->client_txid = cstate->committed_txid;
  cookie->ret_value = D_RW(cstate->last_return_value);
  cookie->ret_size  = cstate->last_return_size;
}

void unlock_cookie()
{
  unlock(&cookies_lock);
}
