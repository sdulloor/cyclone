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

// Rate measurement stuff
static unsigned long *marks;
static unsigned long *completions;
rocksdb::DB* db = NULL;
const char *dir = "data";

void callback(const unsigned char *data,
	      const int len,
	      rpc_cookie_t *cookie)
{
  cookie->ret_value  = malloc(len);
  cookie->ret_size   = len;
  rock_kv_t *rock = (rock_kv_t *)data;
  if(rock->op == OP_PUT) {
    rocksdb::WriteOptions write_options;
    write_options.sync = false;
    rocksdb::Status s = db->Put(write_options, 
				std::to_string(rock->key), 
				rock->value);
    if (!s.ok()){
      BOOST_LOG_TRIVIAL(fatal) << s.ToString();
      exit(-1);
    }
    memcpy(cookie->ret_value, data, len);
  }
  else {
    rock_kv_t *rock_back = (rock_kv_t *)cookie->ret_value;
    std::string val;
    rocksdb::Status s = db->Get(rocksdb::ReadOptions(),
				std::to_string(rock->key),
				&val);
    if(s.IsNotFound()) {
      rock_back->key = ULONG_MAX;
    }
    else {
      rock_back->key = rock->key;
      memcpy(rock_back->value, val.c_str(), 256);
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
  return cookie->log_idx;
}

void gc(rpc_cookie_t *cookie)
{
  free(cookie->ret_value);
}

rpc_callbacks_t rpc_callbacks =  {
  callback,
  gc
};



void opendb(){
  rocksdb::Options options;
  int num_threads=rocksdb_num_threads;
  options.create_if_missing = true;
  //  if (inmem){
  //options.env = rocksdb::NewMemEnv(rocksdb::Env::Default());
  //}
#if 1
    options.PrepareForBulkLoad();
    options.write_buffer_size = 1024 * 1024 * 256;
    options.target_file_size_base = 1024 * 1024 * 512;
    options.IncreaseParallelism(num_threads);
    options.max_background_compactions = num_threads;
    options.max_background_flushes = num_threads;
    options.max_write_buffer_number = num_threads;
    //options.min_write_buffer_number_to_merge = max(num_threads/2, 1);
    options.compaction_style = rocksdb::kCompactionStyleNone;
    //options.memtable_factory.reset(new rocksdb::VectorRepFactory(1000));
    options.env->set_affinity(num_quorums + executor_threads, 
			      num_quorums + executor_threads + num_threads);
#endif
    rocksdb::Status s = rocksdb::DB::Open(options, dir, &db);
    if (!s.ok()){
      BOOST_LOG_TRIVIAL(fatal) << s.ToString().c_str();
      exit(-1);
    }

    // Disable write-ahead log
    
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
  int server_id = atoi(argv[1]);
  cyclone_network_init(argv[4],
		       atoi(argv[6]),
		       atoi(argv[2]),
		       atoi(argv[6]) + num_queues*num_quorums + executor_threads);
  
  
  opendb();
  
  
  dispatcher_start(argv[4], 
		   argv[5], 
		   &rpc_callbacks,
		   server_id, 
		   atoi(argv[2]), 
		   atoi(argv[3]));
}


