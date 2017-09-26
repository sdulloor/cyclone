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
#include<string.h>
#include<stdlib.h>
#include "../core/logging.hpp"
#include "../core/clock.hpp"
#include<stdio.h>
#include <time.h>
#include<unistd.h>
#include <rocksdb/db.h>
#include <rocksdb/options.h>
#include <rocksdb/write_batch.h>
#include "rocksdb.hpp"
#include "fb.hpp"

// Rate measurement stuff
rocksdb::DB* db = NULL;
const int BATCH_SIZE = 100;

void load()
{
  unsigned char value_base[512];
  BOOST_LOG_TRIVIAL(info) << "Start loading.";
  unsigned long ts_heartbeat = rtc_clock::current_time();
  for(unsigned long i=0;i<values;i++) {
    unsigned long keys = NUM_OPS_ARR[i];
    for(unsigned long j=0;j<keys;j++) {
      rocksdb::WriteBatch batch;
      for(int k=j;k<j + BATCH_SIZE && k < keys;k++) {
	unsigned long key = i << 56;
	key += k;
	rocksdb::Slice rdb_key((const char *)&key, 8);
	rocksdb::Slice rdb_value((const char *)&value_base[0], VALUE_SIZE_ARR[i]);
	batch.Put(rdb_key, rdb_value);
      }
      rocksdb::WriteOptions write_options;
      write_options.sync = false;
      write_options.disableWAL = true;
      rocksdb::Status s = db->Write(write_options, 
				    &batch);
      if (!s.ok()){
	BOOST_LOG_TRIVIAL(fatal) << s.ToString();
	exit(-1);
      }
      if(rtc_clock::current_time() > (ts_heartbeat + 60*1000000)) {
	BOOST_LOG_TRIVIAL(info) << "Heartbeat: " << i;
	ts_heartbeat = rtc_clock::current_time();
      }
    }
  }
  BOOST_LOG_TRIVIAL(info) << "Completed loading.";
}


void opendb(){
  rocksdb::Options options;
  options.create_if_missing = true;
  options.error_if_exists   = true;
  auto env = rocksdb::Env::Default();
  env->set_affinity(0, 10); 
  env->SetBackgroundThreads(2, rocksdb::Env::LOW);
  env->SetBackgroundThreads(1, rocksdb::Env::HIGH);
  options.env = env;
  options.PrepareForBulkLoad();
  options.write_buffer_size = 1024 * 1024 * 256;
  options.target_file_size_base = 1024 * 1024 * 512;
  options.max_background_compactions = 2;
  options.max_background_flushes = 1;
  options.max_write_buffer_number = 3;
  //options.min_write_buffer_number_to_merge = max(num_threads/2, 1);
  options.compaction_style = rocksdb::kCompactionStyleNone;
  //options.memtable_factory.reset(new rocksdb::VectorRepFactory(1000));
  rocksdb::Status s = rocksdb::DB::Open(options, preload_dir, &db);
  if (!s.ok()){
    BOOST_LOG_TRIVIAL(fatal) << s.ToString().c_str();
    exit(-1);
  }
}

void closedb()
{
  delete db;
}

int main(int argc, char *argv[])
{
  opendb();
  load();
  closedb();
}

