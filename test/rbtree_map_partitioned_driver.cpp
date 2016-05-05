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
#include "tree_map.hpp"
#include "../core/clock.hpp"
#include "../core/logging.hpp"
#include <libcyclone.hpp>


int main(int argc, const char *argv[]) {
  if(argc != 8) {
    printf("Usage: %s client_id replicas clients sleep_usecs partitions server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  int clients  = atoi(argv[3]);
  unsigned long sleep_time = atol(argv[4]);
  int partitions = atoi(argv[5]);
  void **handles = new void *[partitions];
  char fname_server[50];
  char fname_client[50];
  for(int i=0;i<partitions;i++) {
    sprintf(fname_server, "%s%d.ini", argv[6], i);
    sprintf(fname_client, "%s%d.ini", argv[7], i);
    boost::property_tree::ptree pt_client;
    boost::property_tree::read_ini(fname_client, pt_client);
    handles[i] = cyclone_client_init(me,
				     me % pt_client.get<int>("machines.machines"),
				     replicas,
				     fname_server,
				     fname_client);
  }
  char *buffer = new char[CLIENT_MAXPAYLOAD];
  struct proposal *prop = (struct proposal *)buffer;
  srand(time(NULL));
  int sz;
  void *resp;
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

  double frac_read = 0.5;
  const char *frac_read_env = getenv("RBT_FRAC_READ");
  if(frac_read_env != NULL) {
    frac_read = atof(frac_read_env);
  }
  BOOST_LOG_TRIVIAL(info) << "FRAC_READ = " << frac_read;
  
  total_latency = 0;
  tx_block_cnt  = 0;
  tx_block_begin = rtc_clock::current_time();
  unsigned long tx_begin_time = rtc_clock::current_time();
  srand(tx_begin_time);
  int partition;
  while(true) {
    double coin = ((double)rand())/RAND_MAX;
    if(coin > frac_read) {
      prop->fn = FN_BUMP;
      prop->k_data.key = rand() % keys;
      prop->timestamp = rtc_clock::current_time();
      prop->src       = me;
      prop->order     = (order++);
      partition = prop->k_data.key % partitions;
      sz = make_rpc(handles[partition],
		    buffer,
		    sizeof(struct proposal),
		    &resp,
		    ctr[partition],
		    0);
      ctr[partition]++;
      tx_block_cnt++;
    }
    else {
      prop->fn = FN_LOOKUP;
      prop->k_data.key = rand() % keys;
      prop->timestamp = rtc_clock::current_time();
      prop->src       = me;
      prop->order     = (order++);
      partition = prop->k_data.key % partitions;
      sz = make_rpc(handles[partition],
		    buffer,
		    sizeof(struct proposal),
		    &resp,
		    ctr[partition],
		    RPC_FLAG_RO);
      ctr[partition]++;
      tx_block_cnt++;
    }
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
