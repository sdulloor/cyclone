#include "common.hpp"
#include<libcyclone.hpp>
#include "../core/logging.hpp"

volatile unsigned long cookies_lock = 0;
cookies_t *cookies_root = NULL;
PMEMobjpool *cookies_pool = NULL;

static void clflush(void *ptr, int size)
{
  return;
  char *x = (char *)ptr;
  while(size > 64) {
    asm volatile("clflush %0"::"m"(*x));
    x += 64;
    size -=64;
  }
  asm volatile("clflush %0;mfence"::"m"(*x));
}

void begin_tx()
{
  int e = pmemobj_tx_begin(cookies_pool, NULL, TX_LOCK_NONE);
  if(e != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to start tx errno=" << e;
    exit(-1);
  }
}

void init_cstate(PMEMobjpool *pop, PMEMoid *cs)
{
  int aligned_size = sizeof(struct client_state_st);
  aligned_size = ((aligned_size + 63)/64)*64;
  *cs = pmemobj_tx_alloc(aligned_size, CSTATE_TYPE_NUM);
  struct client_state_st *cnewp = (struct client_state_st *)pmemobj_direct(*cs);
  cnewp->committed_txid = 0;
  cnewp->size = 0;
  clflush(cnewp, sizeof(struct client_state_st));
}


// This function must be executed in the context of a tx
static void mark_done(rpc_cookie_t *cookie)
{
  PMEMoid cstate_old = cookies_root->client_state[cookie->client_id].state;
  pmemobj_tx_free(cstate_old);
  int aligned_size = sizeof(struct client_state_st) + cookie->ret_size;
  aligned_size = ((aligned_size + 63)/64)*64;
  PMEMoid cstate_new = pmemobj_tx_alloc(aligned_size, CSTATE_TYPE_NUM);
  struct client_state_st *cnewp = (struct client_state_st *)pmemobj_direct(cstate_new);
  cnewp->committed_txid = cookie->client_txid;
  cnewp->size = cookie->ret_size;
  memcpy(cnewp + 1, cookie->ret_value, cookie->ret_size);
  clflush(cnewp, sizeof(struct client_state_st) + cookie->ret_size);
  pmemobj_tx_add_range_direct(&cookies_root->client_state[cookie->client_id].state, 
			      sizeof(PMEMoid));
  cookies_root->client_state[cookie->client_id].state = cstate_new;
}

void commit_tx(void *handle, rpc_cookie_t *cookie)
{
  // Idempotent state changes
  if(pmemobj_tx_stage() == TX_STAGE_WORK) {
    mark_done(cookie);
    pmemobj_tx_commit();
  }
  pmemobj_tx_end();
}

void abort_tx(void *handle)
{
  // Idempotent state changes
  if(pmemobj_tx_stage() == TX_STAGE_WORK) {
    pmemobj_tx_abort(0);
  }
  pmemobj_tx_end();
}

void init_cookie_system(PMEMobjpool *pool, cookies_t *root)
{
  cookies_pool = pool;
  cookies_root = root;
}

void get_cookie(rpc_cookie_t *cookie)
{
  struct client_state_st *cstate = (struct client_state_st *)
    pmemobj_direct(cookies_root->client_state[cookie->client_id].state);
  cookie->client_txid = cstate->committed_txid;
  cookie->ret_size  = cstate->size;
  cookie->ret_value = (void *)(cstate + 1);

}

void get_lock_cookie(rpc_cookie_t *cookie)
{
  // NA
}

void unlock_cookie()
{
  // NA
}
