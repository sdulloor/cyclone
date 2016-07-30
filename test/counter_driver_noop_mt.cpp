/*
 * Copyright (c) 2015, Intel Corporation
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 *     * Redistributions of source code must retain the above copyright
 *       notice, this list of conditions and the following disclaimer.
 *
 *     * Redistributions in binary form must reproduce the above copyright
 *       notice, this list of conditions and the following disclaimer in
 *       the documentation and/or other materials provided with the
 *       distribution.
 *
 *     * Neither the name of Intel Corporation nor the names of its
 *       contributors may be used to endorse or promote products derived
 *       from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT
 * LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR
 * A PARTICULAR PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT
 * OWNER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
 * SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT
 * LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES; LOSS OF USE,
 * DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY
 * THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE
 * OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#include <stdio.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <assert.h>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/property_tree/ptree.hpp>
#include "counter.hpp"
#include "../core/clock.hpp"
#include "../core/logging.hpp"
#include <libcyclone.hpp>
#include <rte_launch.h>

int driver(void *arg);

typedef struct driver_args_st {
  int me; 
  int mc;
  int replicas;
  int clients;
  int partitions;
  void **handles;
  void operator() ()
  {
    (void)driver((void *)this);
  }
} driver_args_t;

int driver(void *arg)
{
  driver_args_t *dargs = (driver_args_t *)arg;
  int me = dargs->me; 
  int mc = dargs->mc;
  int replicas = dargs->replicas;
  int clients = dargs->clients;
  int partitions = dargs->partitions;
  void **handles = dargs->handles;
  char *buffer = new char[DISP_MAX_MSGSIZE];
  struct proposal *prop = (struct proposal *)buffer;
  srand(time(NULL));
  int sz;
  struct proposal *resp;
  unsigned long order = 0;
  unsigned long tx_block_cnt   = 0;
  unsigned long tx_block_begin = rtc_clock::current_time();
  unsigned long total_latency  = 0;
  
  int ctr[partitions];
  for(int i=0;i<partitions;i++) {
    BOOST_LOG_TRIVIAL(info) << "Connecting to quorum " << i;
    ctr[i] = get_last_txid(handles[i]) + 1;
    BOOST_LOG_TRIVIAL(info) << "Done";
  }

  unsigned long keys = 10000;
  const char *keys_env = getenv("RBT_KEYS");
  if(keys_env != NULL) {
    keys = atol(keys_env);
  }
  BOOST_LOG_TRIVIAL(info) << "KEYS = " << keys;

  total_latency = 0;
  tx_block_cnt  = 0;
  tx_block_begin = rtc_clock::current_time();
  unsigned long tx_begin_time = rtc_clock::current_time();
  srand(tx_begin_time);
  int partition;
  while(true) {
    uint64_t key = rand() % keys;
    partition = key % partitions;
    /*
    prop->fn = FN_NOOP;
    int rpc_flags = 0;
    //prop->fn = FN_NOOP_RO;
    //int rpc_flags = RPC_FLAG_RO;
    
    prop->k_data.key = key;
    sz = make_rpc(handles[partition],
		  buffer,
		  sizeof(struct proposal),
		  (void **)&resp,
		  ctr[partition],
		  rpc_flags);
    */
    int rpc_flags = 0;
    // int rpc_flags = RPC_FLAG_RO;
    sz = make_noop_rpc(handles[partition], ctr[partition], rpc_flags);
    ctr[partition]++;
    tx_block_cnt++;
    /*
    if(sz != sizeof(struct proposal)) {
      BOOST_LOG_TRIVIAL(fatal) << "Invalid response";
      exit(-1);
    }
    if(resp->code != CODE_OK) {
      BOOST_LOG_TRIVIAL(fatal) << "Improper response";
      exit(-1);
    }
    */
    if(tx_block_cnt > 5000) {
      total_latency = (rtc_clock::current_time() - tx_begin_time);
      BOOST_LOG_TRIVIAL(info) << "LOAD = "
			      << ((double)1000000*tx_block_cnt)/total_latency
			      << " tx/sec "
			      << "LATENCY = "
			      << ((double)total_latency)/tx_block_cnt
			      << " us ";
      tx_begin_time = rtc_clock::current_time();
      tx_block_cnt   = 0;
      total_latency  = 0;
    }
  }
  return 0;
}

int main(int argc, const char *argv[]) {
  if(argc != 9) {
    printf("Usage: %s client_id_start client_id_stop mc replicas clients partitions server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  
  int client_id_start = atoi(argv[1]);
  int client_id_stop  = atoi(argv[2]);
  cyclone_client_global_init(client_id_stop - client_id_start);
  driver_args_t *dargs;
  void **prev_handles;
  
  for(int me = client_id_start; me < client_id_stop; me++) {
    dargs = (driver_args_t *) malloc(sizeof(driver_args_t));
    dargs->me = me;
    dargs->mc = atoi(argv[3]);
    dargs->replicas = atoi(argv[4]);
    dargs->clients  = atoi(argv[5]);
    dargs->partitions = atoi(argv[6]);
    dargs->handles = new void *[dargs->partitions];
    char fname_server[50];
    char fname_client[50];
    for(int i=0;i<dargs->partitions;i++) {
      sprintf(fname_server, "%s%d.ini", argv[7], i);
      sprintf(fname_client, "%s%d.ini", argv[8], i);
      if(me == client_id_start) {
	dargs->handles[i] = cyclone_client_init(me,
						dargs->mc,
						dargs->replicas,
						fname_server,
						fname_client);
      }
      else {
	dargs->handles[i] = cyclone_client_dup(prev_handles[i], me);
      }
    }
    if(me == client_id_start) {
      prev_handles = dargs->handles;
    }
#if defined(DPDK_STACK)
  int e = rte_eal_remote_launch(driver, dargs, 1 + me - client_id_start);
  if(e != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to launch driver on remote lcore";
    exit(-1);
  }
#else
  if(me != (client_id_stop - 1))
    new boost::thread(boost::ref(*dargs));
#endif  
  }
#if defined(DPDK_STACK)
  rte_eal_mp_wait_lcore();
#else
  (*darg)();
#endif
}
