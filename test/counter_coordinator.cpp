// Implement a distrbuted transaction co-ordinator for the red black tree
#include "counter.hpp"
#include <stdio.h>
#include <stdlib.h>
#include "../core/logging.hpp"
#include "common.hpp"

TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static TOID(uint64_t) txnum;
void **quorum_handles;
int me;
int quorums;
int clients;
volatile int *ctr;

typedef struct heap_root_st {
  TOID(uint64_t) txnum;
  cookies_t the_cookies;
}heap_root_t;


TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  TOID(char) store;
  heap_root_t *heap_root;
  if(TOID_IS_NULL(recovered)) {
    store = TX_ALLOC(char, sizeof(heap_root_t));
    heap_root = (heap_root_t *)D_RW(store);
    heap_root->txnum = TX_ALLOC(uint64_t, sizeof(uint64_t));
    *D_RW(heap_root->txnum) = 1;
    for(int i = 0;i < MAX_CLIENTS;i++) {
      heap_root->the_cookies.client_state[i].committed_txid    = 0UL;
      heap_root->the_cookies.client_state[i].last_return_size  = 0;
      TOID_ASSIGN(heap_root->the_cookies.client_state[i].last_return_value, OID_NULL);
    }
    heap_root->the_cookies.applied_raft_idx = -1;
    heap_root->the_cookies.applied_raft_term  = -1;
    txnum = heap_root->txnum;
    init_cookie_system(state, &heap_root->the_cookies);
    return store; 
  }
  else {
    heap_root = (heap_root_t *)D_RW(recovered);
    txnum = heap_root->txnum;
    init_cookie_system(state, &heap_root->the_cookies);
    return recovered;
  }
}


void* leader_callback(const unsigned char *data,
		      const int len,
		      unsigned char **follower_data,
		      int *follower_data_size, 
		      rpc_cookie_t *rpc_cookie)
{
  rbtree_tx_t * tx = (rbtree_tx_t *)data;
  begin_tx();
  *follower_data = (unsigned char *)malloc(sizeof(int));
  *follower_data_size = sizeof(int);
  rpc_cookie->ret_value = (int *)malloc(sizeof(int));
  rpc_cookie->ret_size  = sizeof(int);

  struct proposal *p = (proposal *)(tx + 1);
  struct proposal* resp;
  cookie_t cookie;
  cookie.txnum   = *D_RO(txnum);
  cookie.index   = 0;
  cookie.success = 1; // Assume success
  for(int i=0;i<tx->steps;) {
    cookie.index = i;
    p = ((proposal *)(tx + 1)) + i;
    int partition = get_key(p) % quorums;
    memcpy(&p->cookie, &cookie, sizeof(cookie_t));
    int sz = make_rpc(quorum_handles[partition],
		      p,
		      sizeof(struct proposal),
		      (void **)&resp,
		      ctr[partition],
		      0);
    if(sz == RPC_EOLD) {
      for(partition=0;partition<quorums;partition++) {
	int txid = get_last_txid(quorum_handles[partition]);
	if(txid >= ctr[partition]) {
	  sz = get_response(quorum_handles[partition],
			    (void **)&resp,
			    txid);
	  if(sz == RPC_EOLD) {
	    // Another leader !
	    pmemobj_tx_abort(-1);
	    // This is don't care since we will fail
	    *(int *)*follower_data     = 0;
	    *(int *)rpc_cookie->ret_value  = 0;
	    return NULL;
	  }
	  if(resp->cookie.txnum != *D_RO(txnum)) {
	    pmemobj_tx_abort(-1);
	    // This is don't care since we will fail
	    *(int *)*follower_data     = 0;
	    *(int *)rpc_cookie->ret_value  = 0;
	    return NULL;
	  }
	  if(resp->cookie.success == 0) {
	    cookie.success = 0;
	  }
	  if(resp->cookie.index > i) {
	    i = resp->cookie.index + 1;
	  }
	  ctr[partition] = txid + 1;
	}
      }
      continue;
    }
    memcpy(&cookie, &resp->cookie, sizeof(cookie_t));
    ctr[partition]++;
    i++;
  }
  *(int *)*follower_data     = cookie.success;
  *(int *)rpc_cookie->ret_value  = cookie.success;
  return NULL;
}  

void* follower_callback(const unsigned char *data,
			const int len,
			unsigned char *follower_data,
			int follower_data_size, 
			rpc_cookie_t *rpc_cookie)
{
  begin_tx();
  rpc_cookie->ret_value = malloc(sizeof(int));
  rpc_cookie->ret_size  = sizeof(int);
  *(int *)rpc_cookie->ret_value = *(int *)follower_data;
  TX_ADD(txnum);
  *D_RW(txnum) = *D_RO(txnum) + 1;
  rbtree_tx_t *tx = (rbtree_tx_t *)data;
  struct proposal *p = (proposal *)(tx + 1);
  for(int i=0;i<tx->steps;i++,p++) {
    int partition = get_key(p) % quorums;
    ctr[partition]++;
  }
  return NULL;
}


void gc(void *data)
{
  free(data);
}

rpc_callbacks_t rpc_callbacks =  {
  NULL,
  leader_callback,
  follower_callback,
  get_cookie,
  get_lock_cookie,
  unlock_cookie,
  gc,
  nvheap_setup,
  commit_tx,
  abort_tx
};

int main(int argc, char *argv[])
{
  if(argc != 8) {
    printf("Usage: %s coord_id mc clients partitions replicas server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  int partitions = atoi(argv[4]);
  quorums = partitions;
  int replicas   = atoi(argv[5]);
  int coord_id   = atoi(argv[1]);
  quorum_handles = new void *[partitions];
  ctr = new int[partitions];
  char fname_server[50];
  char fname_client[50];
  clients  = atoi(argv[3]);
  cyclone_client_global_init();
  for(int i=0;i<partitions;i++) {
    sprintf(fname_server, "%s%d.ini", argv[6], i);
    sprintf(fname_client, "%s%d.ini", argv[7], i);
    quorum_handles[i] = cyclone_client_init(clients - 1,
					    atoi(argv[2]),
					    replicas,
					    fname_server,
					    fname_client);
    ctr[i] = get_last_txid(quorum_handles[i]) + 1;
  }
  sprintf(fname_server, "%s%d.ini", argv[6], partitions);
  sprintf(fname_client, "%s%d.ini", argv[7], partitions);
  dispatcher_start(fname_server, 
		   fname_client, 
		   &rpc_callbacks,
		   coord_id,
		   replicas, 
		   clients);
}
