// Implement a distrbuted transaction co-ordinator for the red black tree

TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static TOID(uint64_t) txid;
void **quorum_handles;
int me;
int quorums;

int ctr[partitions];

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


int leader_callback(const unsigned char *data,
		    const int len,
		    unsigned char **follower_data,
		    int * follower_data_size, 
		    void **return_value)
{
  rbtree_tx_t * tx = (rbtree_tx_t *)data;
  costat *rep;
  *follower_data = rep = (costat *)malloc(sizeof(costat));
  *follower_data_size = sizeof(costat);
  rep->tx_status  = 1;
  struct kv *info;
  struct k* del_info;
  // Acquire locks
  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal resp;
    req.fn = FN_LOCK;
    req.kv_data = *info;
    sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  &resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }

  for(int i=0;i<tx->num_versions;i++) {
    // No version check
  }

  for(int i=0;i<tx->num_inserts;i++) {
    info = inserts_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal resp;
    req.fn = FN_INSERT;
    req.kv_data = *info;
    sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  &resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }

  for(int i=0;i<tx->num_deletes;i++) {
    del_info = deletes_list(tx, i);
    int partition = del_info->key % quorums;
    struct proposal req;
    struct proposal resp;
    req.fn = FN_DELETE;
    req.kv_data = *info;
    sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  &resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }

  for(int i=0;i<tx->num_locks;i++) {
    info = locks_list(tx, i);
    info = locks_list(tx, i);
    int partition = info->key % quorums;
    struct proposal req;
    struct proposal resp;
    req.fn = FN_UNLOCK;
    req.kv_data = *info;
    sz = make_rpc(quorum_handles[partition],
		  &req,
		  sizeof(struct proposal),
		  &resp,
		  ctr[partition],
		  0);
    ctr[partition]++;
  }
  return 0;
 fail:
  return 0;
}

int follower_callback(const unsigned char *data,
		      const int len,
		      unsigned char *follower_data,
		      int follower_data_size, 
		      void **return_value)
{
  struct coordinator_status stat =
    (struct coordinator_status *)follower_data;
  TX_ADD(txid);
  uint64_t *txidp = pmemobj_direct(txid);
  *txidp = *txidp + stat->delta_txid;
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
  if(argc != 9) {
    printf("Usage: %s coord_id coord_replicas clients partitions replicas coord_config_prefix server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  quorum_handles = new void *[partitions];
  char fname_server[50];
  char fname_client[50];
  int clients  = atoi(argv[3]);
  for(int i=0;i<partitions;i++) {
    sprintf(fname_server, "%s%d.ini", argv[7], i);
    sprintf(fname_client, "%s%d.ini", argv[8], i);
    quorum_handles[i] = cyclone_client_init(clients - 1,
					    coord_id,
					    replicas,
					    clients,
					    fname_server,
					    fname_client);
  }
  dispatcher_start(argv[6], argv[8], callback, callback_leader,
		   callback_follower, gc, nvheap_setup, argv[1],
		   argv[2], clients);
}
