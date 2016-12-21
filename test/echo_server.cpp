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

// Rate measurement stuff
static unsigned long *marks;
static unsigned long *completions;


int callback(const unsigned char *data,
	       const int len,
	       rpc_cookie_t *cookie)
{
  cookie->ret_value  = NULL;
  cookie->ret_size   = 0;
  cookie->ret_value  = malloc(len);
  cookie->ret_size   = len;
  memcpy(cookie->ret_value, data, len);
  /*
  if((++completions[cookie->core_id]) >= 1000000) {
    BOOST_LOG_TRIVIAL(info) << "Completion rate = "
			    << ((double)completions[cookie->core_id])
      /(rtc_clock::current_time() - marks[cookie->core_id]);
    completions[cookie->core_id] = 0;
    marks[cookie->core_id] = rtc_clock::current_time();
  }
  */
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
		       num_queues*num_quorums + executor_threads);
  dispatcher_start(argv[4], 
		   argv[5], 
		   &rpc_callbacks,
		   server_id, 
		   atoi(argv[2]), 
		   atoi(argv[3]));
}


