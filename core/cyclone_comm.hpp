#ifndef _CYCLONE_COMM_
#define _CYCLONE_COMM_
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "logging.hpp"
#include <unistd.h>
#include "clock.hpp"
#include "tuning.hpp"

/* Cyclone max message size */
const int MSG_MAXSIZE  = 3000; // Maximum user data in pkt

#include "cyclone_comm_dpdk.hpp"

class quorum_switch {
  int* replicas;
public:
  quorum_switch(boost::property_tree::ptree *cluster, boost::property_tree::ptree *quorum)
  {
    std::stringstream key;
    std::stringstream addr;

    key.str("");key.clear();
    key << "quorum.replicas";
    int replica_cnt = quorum->get<int>(key.str().c_str());
    replicas = (int *)malloc(replica_cnt*sizeof(int));
    for(int i=0;i<replica_cnt;i++) {
      key.str("");key.clear();
      key << "quorum.replica" << i;
      replicas[i] = quorum->get<int>(key.str().c_str());
    }
  }

  int replica_mc(int index)
  {
    return replicas[index];
  }

  ~quorum_switch()
  {
    // TBD
  }
};

#endif
