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


#include<assert.h>
#include<errno.h>
#include<libcyclone.hpp>
#include<string.h>
#include<stdlib.h>
#include "../core/logging.hpp"
#include "../core/clock.hpp"
#include<stdio.h>
#include <time.h>
#include<unistd.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include "rocksdb.hpp"
#include <rocksdb/write_batch.h>
#include "fb.hpp"

// Rate measurement stuff
static unsigned long *marks;
static unsigned long *completions;
rocksdb::DB* db = NULL;
static void *logs[executor_threads];

typedef struct batch_barrier_st {
  volatile unsigned long batch_barrier[2];
  volatile int batch_barrier_sense;
} batch_barrier_t;

static batch_barrier_t barriers[executor_threads];

static void barrier(batch_barrier_t *barrier,
		    int thread_id, 
		    unsigned long mask, 
		    bool leader)
{
  int sense = barrier->batch_barrier_sense;
  __sync_fetch_and_or(&barrier->batch_barrier[sense], 1UL << thread_id);
  if(leader) {
    while(barrier->batch_barrier[sense] != mask);
    barrier->batch_barrier_sense  = 1 - barrier->batch_barrier_sense;
    barrier->batch_barrier[sense] = 0;
  }
  else {
    while(barrier->batch_barrier[sense] != 0);
  }
}

void callback(const unsigned char *data,
	      const int len,
	      rpc_cookie_t *cookie)
{
  fb_kv_t *request = (fb_kv_t *)data;

  if(request->op == OP_PUT) {
    rocksdb::WriteOptions write_options;
    if(use_rocksdbwal) {
      write_options.sync       = true;
      write_options.disableWAL = false;
    }
    else {
      write_options.sync       = false;
      write_options.disableWAL = true;
    }
    rocksdb::Slice key((const char *)&request->key, 8);
    rocksdb::Slice value((const char *)(request + 1), len - sizeof(fb_kv_t));
    rocksdb::Status s = db->Put(write_options, 
				key,
				value);
    if (!s.ok()){
      BOOST_LOG_TRIVIAL(fatal) << s.ToString();
      exit(-1);
    }
    cookie->ret_value  = malloc(sizeof(fb_kv_t));
    cookie->ret_size   = sizeof(fb_kv_t);
    fb_kv_t *rock_back = (fb_kv_t *)cookie->ret_value;
    rock_back->key = request->key;
  }
  else {
    rocksdb::Slice key((const char *)&request->key, 8);
    std::string value;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(),
				key,
				&value);
    if(s.IsNotFound()) {
      cookie->ret_value  = malloc(sizeof(fb_kv_t));
      cookie->ret_size   = sizeof(fb_kv_t);
      fb_kv_t *rock_back = (fb_kv_t *)cookie->ret_value;
      rock_back->key = ULONG_MAX;
    }
    else {
      cookie->ret_value  = malloc(sizeof(fb_kv_t) + value.length());
      cookie->ret_size   = sizeof(fb_kv_t) + value.length();
      fb_kv_t *rock_back = (fb_kv_t *)cookie->ret_value;
      rock_back->key = request->key;
      memcpy(rock_back + 1, value.c_str(), value.length());
    }
  }

  /*
  if((++completions[cookie->core_id]) >= 1000000) {
    BOOST_LOG_TRIVIAL(info) << "Completion rate = "
			    << ((double)completions[cookie->core_id])
      /(rtc_clock::current_time() - marks[cookie->core_id]);
    completions[cookie->core_id] = 0;
    marks[cookie->core_id] = rtc_clock::current_time();
  }
  */
}

int wal_callback(const unsigned char *data,
		 const int len,
		 rpc_cookie_t *cookie)
{
  if(use_flashlog) {
    int idx = log_append(logs[cookie->core_id],
			 (const char *)data,
			 len, 
			 cookie->log_idx);
    return idx;
  }
  else {
    return cookie->log_idx;
  }
}

void gc(rpc_cookie_t *cookie)
{
  free(cookie->ret_value);
}

rpc_callbacks_t rpc_callbacks =  {
  callback,
  gc,
  wal_callback
};



void opendb(){
  rocksdb::Options options;
  int num_threads=rocksdb_num_threads;
  options.create_if_missing = true;
  options.write_buffer_size = 1024 * 1024 * 256;
  options.target_file_size_base = 1024 * 1024 * 512;
  options.IncreaseParallelism(num_threads);
  options.max_background_compactions = num_threads;
  options.max_background_flushes = num_threads;
  options.max_write_buffer_number = num_threads;
  options.wal_dir = log_dir;
  options.env->set_affinity(num_quorums + executor_threads, 
			    num_quorums + executor_threads + num_threads);
  rocksdb::Status s = rocksdb::DB::Open(options, data_dir, &db);
  if (!s.ok()){
    BOOST_LOG_TRIVIAL(fatal) << s.ToString().c_str();
    exit(-1);
  }
}

int main(int argc, char *argv[])
{
  if(argc != 7) {
    printf("Usage1: %s replica_id replica_mc clients cluster_config quorum_config ports\n", argv[0]);
    exit(-1);
  }
  marks       = (unsigned long *)malloc(executor_threads*sizeof(unsigned long));
  completions = (unsigned long *)malloc(executor_threads*sizeof(unsigned long));
  memset(marks, 0, executor_threads*sizeof(unsigned long));
  memset(completions, 0, executor_threads*sizeof(unsigned long));
  for(int i=0;i<executor_threads;i++) {
    barriers[i].batch_barrier[0] = 0;
    barriers[i].batch_barrier[1] = 0;
    barriers[i].batch_barrier_sense = 0;
  }
  int server_id = atoi(argv[1]);
  cyclone_network_init(argv[4],
		       atoi(argv[6]),
		       atoi(argv[2]),
		       atoi(argv[6]) + num_queues*num_quorums + executor_threads);
  

  char log_path[50];
  for(int i=0;i<executor_threads;i++) {
    sprintf(log_path, "%s/flash_log%d", log_dir, i);
    logs[i] = create_flash_log(log_path);
  }
  
  opendb();
  
  
  dispatcher_start(argv[4], 
		   argv[5], 
		   &rpc_callbacks,
		   server_id, 
		   atoi(argv[2]), 
		   atoi(argv[3]));
}


