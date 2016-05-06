// Implement a distrbuted transaction co-ordinator for the red black tree
#include "counter.hpp"
#include <stdio.h>
#include <stdlib.h>


TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static TOID(uint64_t) txnum;
void **quorum_handles;
int me;
int quorums;
int clients;
volatile int *ctr;

TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  TOID(char) store;
  if(TOID_IS_NULL(recovered)) {
    txnum = TX_ALLOC(uint64_t, sizeof(uint64_t));
    *D_RW(txnum) = 1;
    store = TX_ALLOC(char, sizeof(TOID(uint64_t)));
    memcpy(D_RW(store), &txnum, sizeof(TOID(uint64_t)));
  }
  else {
    memcpy(&txnum, D_RO(store), sizeof(TOID(uint64_t)));
  }
  return store; 
}


int leader_callback(const unsigned char *data,
		    const int len,
		    unsigned char **follower_data,
		    int *follower_data_size, 
		    void **return_value)
{
  rbtree_tx_t * tx = (rbtree_tx_t *)data;
  *follower_data = (unsigned char *)malloc(sizeof(int));
  *follower_data_size = sizeof(int);
  *return_value = (int *)malloc(sizeof(int));

  struct proposal *p = (proposal *)(tx + 1);
  struct proposal resp;
  cookie_t cookie;
  cookie.txnum   = *D_RO(txnum);
  cookie.index   = 0;
  cookie.success = 1; // Assume success
  for(int i=0;i<tx->steps;) {
    int partition = get_key(p) % quorums;
    cookie.index = i;
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
	  if(resp.cookie.txnum != *D_RO(txnum)) {
	    pmemobj_tx_abort(-1);
	  }
	  if(!resp.cookie.success) {
	    cookie.success = 0;
	  }
	  if(resp.cookie.index > i) {
	    i = resp.cookie.index + 1;
	  }
	  ctr[partition] = txid + 1;
	}
      }
      continue;
    }
    memcpy(&cookie, &resp.cookie, sizeof(cookie_t));
    ctr[partition]++;
    i++;
    p++;
  }
  *(int *)*follower_data = cookie.success;
  *(int *)*return_value  = cookie.success;
  return sizeof(int);
}  

int follower_callback(const unsigned char *data,
		      const int len,
		      unsigned char *follower_data,
		      int follower_data_size, 
		      void **return_value)
{
  *return_value = malloc(sizeof(int));
  *(int *)*return_value = *(int *)follower_data;
  TX_ADD(txnum);
  *D_RW(txnum) = *D_RO(txnum) + 1;
  rbtree_tx_t *tx = (rbtree_tx_t *)data;
  struct proposal *p = (proposal *)(tx + 1);
  for(int i=0;i<tx->steps;i++,p++) {
    int partition = get_key(p) % quorums;
    ctr[partition]++;
  }
  return sizeof(int);
}


void gc(void *data)
{
  free(data);
}

int main(int argc, char *argv[])
{
  if(argc != 10) {
    printf("Usage: %s coord_id coord_replicas clients partitions replicas coord_config coord_client_config server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  int partitions = atoi(argv[4]);
  quorums = partitions;
  int replicas   = atoi(argv[5]);
  int coord_id   = atoi(argv[1]);
  int coord_replicas = atoi(argv[2]);
  quorum_handles = new void *[partitions];
  ctr = new int[partitions];
  char fname_server[50];
  char fname_client[50];
  clients  = atoi(argv[3]);
  for(int i=0;i<partitions;i++) {
    sprintf(fname_server, "%s%d.ini", argv[8], i);
    sprintf(fname_client, "%s%d.ini", argv[9], i);
    quorum_handles[i] = cyclone_client_init(clients - 1,
					    coord_id,
					    replicas,
					    fname_server,
					    fname_client);
    ctr[i] = get_last_txid(quorum_handles[i]) + 1;
  }
  dispatcher_start(argv[6], argv[7], NULL, leader_callback,
		   follower_callback, gc, nvheap_setup, coord_id,
		   coord_replicas, clients);
}
