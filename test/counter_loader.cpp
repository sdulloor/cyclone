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
#include "../core/tuning.hpp"
#include <libcyclone.hpp>


int main(int argc, const char *argv[]) {
  if(argc != 9) {
    printf("Usage: %s client_id mc replicas clients partitions cluster_config quorum_config_prefix server_ports\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  int mc = atoi(argv[2]);
  int replicas = atoi(argv[3]);
  int clients  = atoi(argv[4]);
  int partitions = atoi(argv[5]);
  void **handles = new void *[partitions];
  char fname_server[50];
  char fname_client[50];
  cyclone_network_init(argv[6], 1, mc, 1);
  for(int i=0;i<partitions;i++) {
    sprintf(fname_client, "%s%d.ini", argv[7], i);
    handles[i] = cyclone_client_init(me,
				     mc,
				     0,
				     argv[6],
				     atoi(argv[8]),
				     fname_client);
  }
  char *buffer = new char[DISP_MAX_MSGSIZE];
  struct proposal *prop = (struct proposal *)buffer;
  srand(time(NULL));
  int sz;
  struct proposal *resp;
  unsigned long order = 0;
  unsigned long tx_block_cnt   = 0;
  unsigned long tx_block_begin = rtc_clock::current_time();
  unsigned long total_latency  = 0;
  
  int ctr[partitions][executor_threads];
  for(int i=0;i<partitions;i++) {
    for(int j=0;j<executor_threads;j++) {
      BOOST_LOG_TRIVIAL(info) << "Connecting to partition " << i << " cpu " << j;
      ctr[i][j] = get_last_txid(handles[i], 0, j) + 1;
      BOOST_LOG_TRIVIAL(info) << "Done";
    }
  }

  unsigned long keys = 10000;
  const char *keys_env = getenv("RBT_KEYS");
  if(keys_env != NULL) {
    keys = atol(keys_env);
  }
  BOOST_LOG_TRIVIAL(info) << "KEYS = " << keys;

  unsigned long keys_start = me*(keys/clients);
  unsigned long keys_stop = (me + 1)*(keys/clients);
  if(me == (clients - 1)) {
    keys_stop = keys;
  }
  
  for(unsigned long i=keys_start;i<keys_stop;i++) {
    prop->fn = FN_INSERT;
    prop->kv_data.key   = i;
    prop->kv_data.value = 0;
    unsigned long tx_begin_time = rtc_clock::current_time();
    int partition = prop->kv_data.key % partitions;
    int quorum    = prop->kv_data.key % num_quorums;
    int core      = prop->kv_data.key % executor_threads;
    sz = make_rpc(handles[partition],
		  buffer,
		  sizeof(struct proposal),
		  (void **)&resp,
		  ctr[partition][core],
		  quorum,
		  core,
		  0);
    ctr[partition][core]++;
    if(sz != sizeof(struct proposal)) {
      BOOST_LOG_TRIVIAL(fatal) << "Invalid response";
      exit(-1);
    }
    if(resp->code != CODE_OK) {
      BOOST_LOG_TRIVIAL(fatal) << "Insert failed";
      exit(-1);
    }
    total_latency += (rtc_clock::current_time() - tx_begin_time);
    tx_block_cnt++;
    if(rtc_clock::current_time() - tx_block_begin >= 10000) {
      BOOST_LOG_TRIVIAL(info) << "INSERTED = "
			      << (i - keys_start + 1)
			      << " LOAD = "
			      << ((double)1000000*tx_block_cnt)/(rtc_clock::current_time() - tx_block_begin)
			      << " tx/sec "
			      << "LATENCY = "
			      << ((double)total_latency)/tx_block_cnt
			      << " us ";
      tx_block_begin = rtc_clock::current_time();
      tx_block_cnt   = 0;
      total_latency  = 0;
    }
    struct proposal *rr;
    prop->fn = FN_LOOKUP;
    prop->k_data.key = i;
    partition = prop->k_data.key % partitions;
    quorum    = prop->kv_data.key % num_quorums;
    core      = prop->kv_data.key % executor_threads;
    sz = make_rpc(handles[partition],
		  buffer,
		  sizeof(struct proposal),
		  (void **)&rr,
		  ctr[partition][core],
		  quorum,
		  core,
		  RPC_FLAG_RO);
    ctr[partition][core]++;
    if(sz != sizeof(struct proposal)) {
      BOOST_LOG_TRIVIAL(fatal) << "Invalid response";
      exit(-1);
    }
    if(rr->code != CODE_OK) {
      BOOST_LOG_TRIVIAL(fatal) << "Key not found";
      exit(-1);
    }
  }
  BOOST_LOG_TRIVIAL(info) << "LOADING COMPLETE";
  return 0;
}
