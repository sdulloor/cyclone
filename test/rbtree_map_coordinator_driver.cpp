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
#include "rbtree_map_coordinator.hpp"
#include "../core/clock.hpp"
#include "../core/logging.hpp"
#include <libcyclone.hpp>


#define KEYS 100

int main(int argc, const char *argv[]) {
  rtc_clock clock;
  if(argc != 10) {
    printf("Usage: %s client_id replicas clients sleep_usecs server_config  client_config partitions quorum_server_prefix quorum_client_prefix\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  int clients  = atoi(argv[3]);
  unsigned long sleep_time = atol(argv[4]);
  void * handle = cyclone_client_init(me,
				      me,
				      replicas,
				      clients,
				      argv[5],
				      argv[6]);
  int partitions = atoi(argv[7]);
  void **quorum_handles = new void*[partitions];
  int *ctr_array = new int[partitions];
  char fname_server[50];
  char fname_client[50];
  for(int i=0;i<partitions;i++) {
    sprintf(fname_server, "%s%d.ini", argv[8], i);
    sprintf(fname_client, "%s%d.ini", argv[9], i);
    quorum_handles[i] = cyclone_client_init(me,
					    me,
					    replicas,
					    clients,
					    fname_server,
					    fname_client);
    ctr_array[i] = get_last_txid(quorum_handles[i]) + 1;
  }
  char *buffer = new char[CLIENT_MAXPAYLOAD];
  rbtree_tx_t *tx = (rbtree_tx_t *)buffer;
  int sz;
  void *resp;
  unsigned long order = 0;
  unsigned long tx_block_cnt   = 0;
  unsigned long tx_block_begin = clock.current_time();
  unsigned long total_latency  = 0;
  int ctr = get_last_txid(handle) + 1;
  
  while(true) {
    init_tx(tx, 1, 0, 1, 0);
    uint64_t key = me*KEYS + (uint64_t)((KEYS - 1)*(rand()/(1.0*RAND_MAX)));
    struct proposal vquery;
    vquery.fn = FN_GET_VERSION;
    vquery.k_data.key   = me*KEYS + key;
    vquery.timestamp = clock.current_time();
    vquery.src       = me;
    vquery.order     = 0;
    int q = vquery.k_data.key % partitions;
    sz = make_rpc(quorum_handles[q], &vquery, sizeof(struct proposal), &resp,
		  ctr_array[q], 0);
    
    if(sz != sizeof(struct proposal)) {
      fprintf(stderr, "unexepected response to version query of size %d", sz);
      exit(-1);
    }
    ctr_array[q]++;
    struct kv *infolock = locks_list(tx, 0);
    struct kv *infok = inserts_list(tx, 0);
    infolock->key = me*KEYS + key;
    infolock->value = ((struct proposal *)resp)->code;
    infok->key   = me*KEYS + key;
    infok->value = 0xdeadbeef;
    unsigned long tx_begin_time = clock.current_time();
    sz = make_rpc(handle, buffer, size_tx(tx), &resp, ctr, RPC_FLAG_SYNCHRONOUS);
    if(sz != sizeof(tx_client_response)) {
      fprintf(stderr, "unexepected response to version query of size %d", sz);
      exit(-1);
    }
    if(((tx_client_response *)resp)->tx_status != 1) {
      fprintf(stderr, "Tx failed !");
      exit(-1);
    }
    ctr++;
    total_latency += (clock.current_time() - tx_begin_time);
    usleep(sleep_time);
    tx_block_cnt++;
    if(clock.current_time() - tx_block_begin >= 10000) {
      BOOST_LOG_TRIVIAL(info) << "LOAD = "
			      << ((double)1000000*tx_block_cnt)/(clock.current_time() - tx_block_begin)
			      << " tx/sec "
			      << "LATENCY = "
			      << ((double)total_latency)/tx_block_cnt
			      << " us ";
      tx_block_begin = clock.current_time();
      tx_block_cnt   = 0;
      total_latency  = 0;
    }
  }
  return 0;
}
