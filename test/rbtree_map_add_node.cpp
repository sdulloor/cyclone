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
  rtc_clock clock;
  if(argc != 9) {
    printf("Usage: %s client_id mc_id replicas clients server_prefix client_prefix partition replica\n", argv[0]); 
    exit(-1);
  }
  int me = atoi(argv[1]);
  int replicas = atoi(argv[3]);
  int clients  = atoi(argv[4]);
  char fname_server[50];
  char fname_client[50];
  sprintf(fname_server, "%s%s.ini", argv[5], argv[7]);
  sprintf(fname_client, "%s%s.ini", argv[6], argv[7]);
  boost::property_tree::ptree pt_client;
  boost::property_tree::read_ini(fname_client, pt_client);
  void* handle = cyclone_client_init(me,
				     atoi(argv[2]),
				     replicas,
				     fname_server,
				     fname_client);
  int ctr = get_last_txid(handle) + 1;
  int sz  = add_node(handle, ctr, atoi(argv[8])); 
  BOOST_LOG_TRIVIAL(info) << "Done, code = " << sz;
  return 0;
}
