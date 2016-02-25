// Implement a distrbuted transaction co-ordinator for the red black tree
#include "rbtree_map_coordinator.hpp"
#include <stdio.h>


TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static TOID(uint64_t) txid;
void **quorum_handles;
int me;
int quorums;
int clients;
int *ctr;

TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  TOID(char) store;
  if(TOID_IS_NULL(recovered)) {
    txid = TX_ALLOC(uint64_t, sizeof(uint64_t));
    *D_RW(txid) = 1;
    store = TX_ALLOC(char, sizeof(TOID(uint64_t)));
    TX_MEMCPY(D_RW(store), &txid, sizeof(TOID(uint64_t)));
  }
  else {
    TX_MEMCPY(&txid, D_RO(store), sizeof(TOID(uint64_t)));
  }
  return store; 
}


int leader_callback_recovery(const unsigned char *data,
			     const int len,
			     unsigned char **follower_data,
			     int *follower_data_size,
			     void **return_value,
			     int *ctr_recovery,
			     int tx_status)
{
  rbtree_tx_t * tx = (rbtree_tx_t *)data;
  costat *rep;
  rep = (costat *)malloc(sizeof(costat));
  *follower_data = (unsigned char *)rep;
  *follower_data_size = sizeof(costat);
  rep->tx_status  = 1;
  struct kv *info;
  struct k* del_info;

  tx_client_response *client_resp;
  *return_value = malloc(sizeof(tx_client_response));
  client_resp = (tx_client_response *)*return_value;
  client_resp->tx_status = tx_status;

  if(tx_status == 0) {
    for(int i=0;i<quorums;i++) {
      ctr[i] = ctr_recovery[i];
    }
    goto cleanup;
  }
  
  // Acquire locks
  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_LOCK;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;

    bool make_call = (ctr_recovery[partition] <= ctr[partition]);
    if(make_call) {
      int sz = make_rpc(quorum_handles[partition],
			&req,
			sizeof(struct proposal),
			(void **)&resp,
			ctr[partition],
			0);
    }
    ctr[partition]++;

    if(make_call && resp->code != req.kv_data.value) {
      // Cleanup and fail
      client_resp->tx_status = 0;
      tx->num_locks = i;
      break;
    }
    info->value = resp->code + 1;
  }

  if(client_resp->tx_status == 0) {
    goto cleanup;
  }
  
  for(int i=0;i<tx->num_versions;i++) {
    info = versions_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_GET_VERSION;
    req.k_data.key = info->key;
    req.src       = clients - 1;
    req.order     = 0;
    bool make_call = (ctr_recovery[partition] <= ctr[partition]);

    if(make_call) {
      int sz = make_rpc(quorum_handles[partition],
			&req,
			sizeof(struct proposal),
			(void **)&resp,
			ctr[partition],
			0);

    }
    ctr[partition]++;
    if(make_call && resp->code != req.kv_data.value) {
      // Cleanup and fail
      client_resp->tx_status = 0;
      break;
    }
  }

  if(client_resp->tx_status == 0) {
    goto cleanup;
  }
  
  for(int i=0;i<tx->num_inserts;i++) {
    info = inserts_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_INSERT;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;

    bool make_call = (ctr_recovery[partition] <= ctr[partition]);
    if(make_call) {
      int sz = make_rpc(quorum_handles[partition],
			&req,
			sizeof(struct proposal),
			(void **)&resp,
			ctr[partition],
			0);
    }
    ctr[partition]++;
  }

  for(int i=0;i<tx->num_deletes;i++) {
    del_info = deletes_list(tx, i);
    int partition = del_info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_DELETE;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;
    bool make_call = (ctr_recovery[partition] <= ctr[partition]);
    if(make_call) {
      int sz = make_rpc(quorum_handles[partition],
			&req,
			sizeof(struct proposal),
			(void **)&resp,
			ctr[partition],
			0);
    }
    ctr[partition]++;
  }

 cleanup:
  
  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_UNLOCK;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;
    int sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  (void **)&resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }

  return sizeof(tx_client_response);
}

int leader_callback(const unsigned char *data,
		    const int len,
		    unsigned char **follower_data,
		    int *follower_data_size, 
		    void **return_value)
{
  rbtree_tx_t * tx = (rbtree_tx_t *)data;
  costat *rep;
  rep = (costat *)malloc(sizeof(costat));
  *follower_data = (unsigned char *)rep;
  *follower_data_size = sizeof(costat);
  rep->tx_status  = 1;
  struct kv *info;
  struct k* del_info;

  tx_client_response *client_resp;
  *return_value = malloc(sizeof(tx_client_response));
  client_resp = (tx_client_response *)*return_value;
  client_resp->tx_status = 1;

  // Acquire locks
  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_LOCK;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;

    int sz = make_rpc(quorum_handles[partition],
		      &req,
		      sizeof(struct proposal),
		      (void **)&resp,
		      ctr[partition],
		      0);
    if(sz == RPC_EOLD) {
      int *recovery_ctr = new int[quorums];
      for(int i=0;i<quorums;i++) {
	recovery_ctr[i] = get_last_txid(quorum_handles[i]) + 1;
      }
      free(rep);
      free(*return_value);
      int recovery_ret_value =
	leader_callback_recovery(data,
				 len,
				 follower_data,
				 follower_data_size,
				 return_value,
				 recovery_ctr,
				 1); // TBD: fix tx status
      delete recovery_ctr;
      return recovery_ret_value;
    }

    ctr[partition]++;
    if(resp->code != req.kv_data.value) {
      // Cleanup and fail
      client_resp->tx_status = 0;
      tx->num_locks = i;
      break;
    }
    info->value = resp->code + 1;
  }

  if(client_resp->tx_status == 0) {
    goto cleanup;
  }
  
  for(int i=0;i<tx->num_versions;i++) {
    info = versions_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_GET_VERSION;
    req.k_data.key = info->key;
    req.src       = clients - 1;
    req.order     = 0;

    int sz = make_rpc(quorum_handles[partition],
		      &req,
		      sizeof(struct proposal),
		      (void **)&resp,
		      ctr[partition],
		      0);

    ctr[partition]++;
    if(resp->code != req.kv_data.value) {
      // Cleanup and fail
      client_resp->tx_status = 0;
      break;
    }
  }

  if(client_resp->tx_status == 0) {
    goto cleanup;
  }
  
  for(int i=0;i<tx->num_inserts;i++) {
    info = inserts_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_INSERT;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;

    int sz = make_rpc(quorum_handles[partition],
		      &req,
		      sizeof(struct proposal),
		      (void **)&resp,
		      ctr[partition],
		      0);

    ctr[partition]++;
  }

  for(int i=0;i<tx->num_deletes;i++) {
    del_info = deletes_list(tx, i);
    int partition = del_info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_DELETE;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;
    int sz = make_rpc(quorum_handles[partition],
		      &req,
		      sizeof(struct proposal),
		      (void **)&resp,
		      ctr[partition],
		      0);
    ctr[partition]++;
  }

 cleanup:
  
  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal *resp;
    req.fn = FN_UNLOCK;
    req.kv_data = *info;
    req.src       = clients - 1;
    req.order     = 0;
    int sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  (void **)&resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }

  return sizeof(tx_client_response);
}

int follower_callback(const unsigned char *data,
		      const int len,
		      unsigned char *follower_data,
		      int follower_data_size, 
		      void **return_value)
{
  struct coordinator_status *stat =
    (struct coordinator_status *)follower_data;
  TX_ADD(txid);
  *D_RW(txid) = *D_RO(txid) + stat->delta_txid; 
  *return_value = malloc(sizeof(int));
  *(int *)*return_value = stat->tx_status;
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
					    clients,
					    fname_server,
					    fname_client);
    ctr[i] = get_last_txid(quorum_handles[i]) + 1;
  }
  dispatcher_start(argv[6], argv[7], NULL, leader_callback,
		   follower_callback, gc, nvheap_setup, coord_id,
		   coord_replicas, clients);
}
