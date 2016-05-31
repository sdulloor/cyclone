#include <string.h>
#include <stdio.h>
#include <libcyclone.hpp>
#include "jarvis_demo.h"

Graph *db;

int main(int argc, char **argv)
{
  // Create a fully connected graph
  const char *db_name = "jarvis_demo_graph";
  db = new Graph(db_name, Graph::Create);
  Transaction tx(*db, Transaction::ReadWrite);
  db->create_index(Graph::NodeIndex, 0, ID_STR, PropertyType::Integer);
  tx.commit();
  for(int src_idx = 0; src_idx < GRAPH_NODES; src_idx++) {
    for(int dst_idx = 0;dst_idx < GRAPH_NODES; dst_idx++) {
      Transaction tx(*db, Transaction::ReadWrite);
      Node &src = get_node(*db, src_idx);
      if(src_idx == dst_idx) {
	src.set_property("Counter", 1);
      }
      Node &dst = get_node(*db, dst_idx);
      db->add_edge(src, dst, 0);
      tx.commit();
    }
  }
  for(int client=0; client < MAX_CLIENTS; client++) {
    Transaction tx(*db, Transaction::ReadWrite);
    Node &client_state = get_node(*db, GRAPH_NODES + client);
    client_state.set_property("committed_txid", 0);
    client_state.set_property("last_return", 0);
    if(client == (MAX_CLIENTS - 1)) {
      Node &global_tx_state = get_node(*db, GRAPH_NODES + MAX_CLIENTS);
      global_tx_state.set_property("raft_idx", -1);
      global_tx_state.set_property("raft_term", -1);
    }
    tx.commit();
  }
}

