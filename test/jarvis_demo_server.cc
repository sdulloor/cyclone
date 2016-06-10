#include "jarvis_demo.h"
#include<libcyclone.hpp>
#include "../core/clock.hpp"

static volatile unsigned long cookies_lock = 0;
Graph *db;
int commit_raft_idx;
int commit_raft_term;
int *commit_txids;

static void lock(volatile unsigned long *lockp)
{
  // TEST + TEST&SET
  do {
    while((*lockp) != 0);
  } while(!__sync_bool_compare_and_swap(lockp, 0, 1));
  __sync_synchronize();
}

static void unlock(volatile unsigned long *lockp)
{
  __sync_synchronize();
  __sync_bool_compare_and_swap(lockp, 1, 0);
  __sync_synchronize();
}

void abort_tx(void *handle)
{
  Transaction *tx = (Transaction *)handle;
  delete tx; // JL is redo-log based
}


// This function must be executed in the context of a tx
void mark_done(rpc_cookie_t *cookie)
{
  Node &client_state = get_node(*db, GRAPH_NODES + cookie->client_id);
  Node &global_state = get_node(*db, GRAPH_NODES + MAX_CLIENTS);
  client_state.set_property("last_return", 1);
  client_state.set_property("committed_txid", cookie->client_txid);
  global_state.set_property("raft_idx", cookie->raft_idx);
  global_state.set_property("raft_term", cookie->raft_term);
  lock(&cookies_lock);
  commit_raft_idx  = cookie->raft_idx;
  commit_raft_term = cookie->raft_term;
  commit_txids[cookie->client_id] = cookie->client_txid;
  unlock(&cookies_lock);
}

void commit_tx(void *handle, rpc_cookie_t *cookie)
{
  Transaction *tx = (Transaction *)handle;
  mark_done(cookie);
  tx->commit();
  delete tx;
}

void get_cookie(rpc_cookie_t *cookie)
{
  lock(&cookies_lock);
  cookie->raft_idx  =  commit_raft_idx;
  cookie->raft_term =  commit_raft_term;
  unlock(&cookies_lock);
}

int dummy = 1;

void get_lock_cookie(rpc_cookie_t *cookie)
{
  lock(&cookies_lock);
  cookie->raft_idx  =  commit_raft_idx;
  cookie->raft_term =  commit_raft_term;
  cookie->client_txid = commit_txids[cookie->client_id];
  cookie->ret_value = &dummy;
  cookie->ret_size  = sizeof(int);
}

void unlock_cookie()
{
  unlock(&cookies_lock);
}

static rtc_clock exec_clock("LOAD_EXEC", 50000);

void* callback(const unsigned char *data,
	       const int len,
	       rpc_cookie_t *cookie)
{

  unsigned long exec_begin_time = rtc_clock::current_time();

  cookie->ret_value = malloc(sizeof(int));
  cookie->ret_size  = sizeof(int);
  *(int *)cookie->ret_value = 1; // OK
  Transaction *tx  = new Transaction(*db, Transaction::ReadWrite);
  int src_idx = *(int *)data;
  int dst_idx = *(int *)(data + sizeof(int));
  Node &src = get_node(*db, src_idx);
  src.set_property("Counter", src.get_property("Counter").int_value() + 1);
  Node &dst = get_node(*db, dst_idx);
  dst.set_property("Counter", dst.get_property("Counter").int_value() + 1);
  exec_clock.sample(rtc_clock::current_time() - exec_begin_time);
  return tx;
}

void gc(void *data)
{
  free(data);
}

TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  
  TOID(char) store;
  TOID_ASSIGN(store, OID_NULL);
  const char *db_name = "jarvis_demo_graph";
  db = new Graph(db_name);
  commit_txids = (int *)malloc(MAX_CLIENTS*sizeof(int));
  Transaction tx(*db, Transaction::ReadOnly);
  Node &global_state = get_node(*db, GRAPH_NODES + MAX_CLIENTS);
  commit_raft_idx  =  global_state.get_property("raft_idx").int_value();
  commit_raft_term =  global_state.get_property("raft_term").int_value();
  tx.commit();
  for(int i=0;i<MAX_CLIENTS;i++) {
    Transaction tx(*db, Transaction::ReadOnly);
    Node &client_state = get_node(*db, GRAPH_NODES + i);
    commit_txids[i] = client_state.get_property("committed_txid").int_value();
    tx.commit();
  }
  return store;
}

rpc_callbacks_t rpc_callbacks =  {
  callback,
  NULL,
  NULL,
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
  if(argc != 4 && argc != 6) {
    printf("Usage1: %s server_id replicas clients \n", argv[0]);
    printf("Usage1: %s server_id replicas clients server_config client_config\n", argv[0]);
    exit(-1);
  }
  int server_id = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  int clients  = atoi(argv[3]);
  if(argc == 4) {
    dispatcher_start("cyclone_test.ini", 
		     "cyclone_test.ini", 
		     &rpc_callbacks,
		     server_id, 
		     replicas, 
		     clients);
  }
  else {
      dispatcher_start(argv[4], 
		       argv[5], 
		       &rpc_callbacks,
		       server_id, 
		       replicas, 
		       clients);
  }
}





