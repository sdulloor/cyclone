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


rtc_clock timer;
#define	MAX_INSERTS 100
static uint64_t nkeys;
static uint64_t keys[MAX_INSERTS];

int main(int argc, const char *argv[]) {
  boost::log::keywords::auto_flush = true;
  if(argc != 2) {
    printf("Usage: %s client_id\n", argv[0]);
    exit(-1);
  }
  int me = atoi(argv[1]);
  void * handle = cyclone_client_init(me, "cyclone_test.ini");
  char *proposal = new char[CLIENT_MAXPAYLOAD];
  srand(time(NULL));
  struct kv insert_data;
  struct k query_data;
  int sz;
  void *resp;
  nkeys = 0;
  for (int i = 0; i < MAX_INSERTS; ++i) {
    insert_data.key = rand();
    insert_data.value = rand();
    *(int *)proposal = FN_INSERT;
    *(struct kv *)(proposal + sizeof(int)) = insert_data; 
    timer.start();
    sz = make_rpc(handle, proposal, sizeof(int) + sizeof(struct kv), &resp);
    timer.stop();
    timer.print("Time: ");
    timer.reset();
    keys[nkeys++] = insert_data.key;
  }
  for (int i = 0; i < MAX_INSERTS; ++i) {
    query_data.key = keys[i];
    *(int *)proposal = FN_LOOKUP;
    *(struct k *)(proposal + sizeof(int)) = query_data; 
    timer.start();
    sz = make_rpc(handle, proposal, sizeof(int) + sizeof(struct k), &resp);
    timer.stop();
    if(sz != sizeof(struct kv)) {
      BOOST_LOG_TRIVIAL(error) << "Key not found !";
    }
    else {
      BOOST_LOG_TRIVIAL(info) << "Success: " << ((struct kv *)resp)->key;
    }
    timer.print("Time: ");
    timer.reset();
  }
  return 0;
}
