// Implement a distrbuted transaction co-ordinator for the red black tree

TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static TOID(uint64_t) txid;

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


int callback(const unsigned char *data,
	     const int len,
	     const int possibly_leader,
	     void **return_value)
{
  rbtree_tx_t *tx = (rbtree_tx_t *)data;
  if(possibly_leader) {
  }
  else {
    
  }
}
