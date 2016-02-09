#ifndef _RBTREE_COORD_
#define _RBTREE_COORD_
#include "tree_map.hpp"

typedef struct rbtree_tx_st {
  int num_locks;
  int num_versions;
  int num_inserts;
  int num_deletes;
  int breadcrumb_txid;
  uint64_t breadcrumb_status;
  char payload[0];
} rbtree_tx_t;

//Deserialize
static struct kv *locks_list(rbtree_tx_t *tx, int index)
{
  return (struct kv *)
    (tx->payload + index*sizeof(struct kv));
}

static struct kv *versions_list(rbtree_tx_t *tx, int index)
{
  return (struct kv *)
    (tx->payload +
     tx->locks*sizeof(struct kv) +
     index*sizeof(struct kv));
}

static struct kv *inserts_list(rbtree_tx_t *tx, int index)
{
  return (struct kv *)
    (tx->payload +
     tx->locks*sizeof(struct kv) +
     tx->versions*sizeof(struct kv) +
     index*sizeof(struct kv));
}

static struct k *deletes_list(rbtree_tx_t *tx, int index)
{
  return (struct kv *)
    (tx->payload +
     tx->locks*sizeof(struct kv) +
     tx->versions*sizeof(struct kv) +
     tx->inserts*sizeof(struct kv) +
     index*sizeof(struct k));
}

static rbtree_tx_t * alloc_tx(int num_locks,
			     int num_inserts,
			     int num_deletes)
{
  int size =
    num_locks * sizeof(struct kv) +
    num_versions * sizeof(struct kv) +
    num_inserts * sizeof(struct kv) +
    num_deletes * sizeof(struct k);
  rbtree_tx_t *tx = malloc(sizeof(rbtree_tx_t) + size);
  return tx;
}

static void init_tx(rbtree_tx_t * tx,
		    int num_locks,
		    int num_inserts,
		    int num_deletes)
{

  tx->num_locks    = num_locks;
  tx->num_versions = num_versions;
  tx->num_inserts  = num_inserts;
  tx->num_deletes  = num_deletes;
}

typedef struct coordination_status {
  int tx_status;  // 0 == fail, 1 == success
  int delta_txid;
}costat;
#endif
