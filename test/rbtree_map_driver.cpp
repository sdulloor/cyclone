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
#include "tree_map.hpp"
#include "../core/clock.hpp"
#include<boost/log/trivial.hpp>
#include <boost/log/utility/setup.hpp>
#include <libcyclone.hpp>


void trace_send_cmd(void *data, const int size)
{

}

void trace_recv_cmd(void *data, const int size)
{

}

void trace_pre_append(void *data, const int size)
{

}

void trace_post_append(void *data, const int size)
{

}

void trace_send_entry(void *data, const int size)
{

}

void trace_recv_entry(void *data, const int size)
{

}

#define KEYS 100

int main(int argc, const char *argv[]) {
  boost::log::keywords::auto_flush = true;
  rtc_clock clock;
  if(argc != 2) {
    printf("Usage: %s client_id\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  void * handle = cyclone_client_init(me, "cyclone_test.ini");
  char *buffer = new char[CLIENT_MAXPAYLOAD];
  struct proposal *prop = (struct proposal *)buffer;
  srand(time(NULL));
  int sz;
  void *resp;
  unsigned long order = 0;
  unsigned long tx_block_cnt   = 0;
  unsigned long tx_block_begin = clock.current_time();

  while(true) {
    for(int i=0;i<KEYS;i++) {
      prop->fn = FN_INSERT;
      prop->kv_data.key   = me*KEYS + i;
      prop->kv_data.value = me*KEYS + i;
      prop->timestamp = clock.current_time();
      prop->src       = me;
      prop->order     = (order++);
      sz = make_rpc(handle, buffer, sizeof(struct proposal), &resp);
      tx_block_cnt++;
      if(clock.current_time() - tx_block_begin >= 10000) {
	BOOST_LOG_TRIVIAL(info) << "BLOCK THROUGHPUT = "
				<< ((double)1000000*tx_block_cnt)/(clock.current_time() - tx_block_begin)
				<< " tx/sec ";
	tx_block_begin = clock.current_time();
	tx_block_cnt   = 0;
      }
    }
    for(int i=0;i<KEYS;i++) {
      prop->fn = FN_DELETE;
      prop->k_data.key = me*KEYS + i;
      prop->timestamp = clock.current_time();
      prop->src       = me;
      prop->order     = (order++);
      sz = make_rpc(handle, buffer, sizeof(struct proposal), &resp);
      tx_block_cnt++;
      if(clock.current_time() - tx_block_begin >= 10000) {
	BOOST_LOG_TRIVIAL(info) << "BLOCK THROUGHPUT = "
				<< ((double)1000000*tx_block_cnt)/(clock.current_time() - tx_block_begin)
				<< " tx/sec ";
	tx_block_begin = clock.current_time();
	tx_block_cnt   = 0;
      }
    }
  }
  return 0;
}
