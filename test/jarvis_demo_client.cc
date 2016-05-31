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
#include "../core/clock.hpp"
#include "../core/logging.hpp"
#include <libcyclone.hpp>
#include <jarvis.h>
#include "jarvis_demo.h"

int main(int argc, const char *argv[]) {
  if(argc != 7) {
    printf("Usage: %s client_id replicas clients sleep_usecs server_config_prefix client_config_prefix\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  char fname_server[50];
  char fname_client[50];
  sprintf(fname_server, "%s%d.ini", argv[5], 0);
  sprintf(fname_client, "%s%d.ini", argv[6], 0);
  boost::property_tree::ptree pt_client;
  boost::property_tree::read_ini(fname_client, pt_client);
  void *handle = cyclone_client_init(me,
				     me % pt_client.get<int>("machines.machines"),
				     replicas,
				     fname_server,
				     fname_client);
  srand(time(NULL));
  char *buffer = new char[CLIENT_MAXPAYLOAD];
  BOOST_LOG_TRIVIAL(info) << "Connecting to quorum ";
  int ctr = get_last_txid(handle) + 1;
  BOOST_LOG_TRIVIAL(info) << "Done";
 
  unsigned long tx_block_cnt   = 0;
  unsigned long tx_begin_time = rtc_clock::current_time();
  unsigned long total_latency  = 0;
  while(true) {
    void *resp;
    *(int *)buffer = rand() % GRAPH_NODES;
    *(int *)(buffer + sizeof(int)) = rand() % GRAPH_NODES;
    int sz = make_rpc(handle,
		      buffer,
		      2*sizeof(int),
		      (void **)&resp,
		      ctr,
		      0);
    ctr++;
    tx_block_cnt++;
    if(sz != sizeof(int)) {
      BOOST_LOG_TRIVIAL(fatal) << "Invalid response";
      exit(-1);
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
