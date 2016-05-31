#ifndef _JARVIS_DEMO_
#define JARVIS_DEMO
#include "jarvis.h"

using namespace Jarvis;


const int GRAPH_NODES = 100;
static const char ID_STR[] = "JARVIS_DEMO";

static Node &get_node(Graph &db, long long id)
{
    NodeIterator nodes = db.get_nodes(0,
                             PropertyPredicate(ID_STR, PropertyPredicate::Eq, id));
    if (nodes) return *nodes;
    // Node not found; add it
    Node &node = db.add_node(0);
    node.set_property(ID_STR, id);
    return node;
}

#endif
