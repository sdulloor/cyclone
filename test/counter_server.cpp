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
extern "C" {
#include "../nvml.git/src/examples/libpmemobj/tree_map/rbtree_map.h"
}
#include "counter.hpp"

TOID_DECLARE(uint64_t, TOID_NUM_BASE);

static unsigned long server_id;
static TOID(struct rbtree_map) the_tree;
static PMEMobjpool *pop;

typedef struct heap_root_st {
  TOID(struct rbtree_map) the_tree;
}heap_root_t;


TOID(uint64_t) new_store_item(uint64_t val)
{
  TOID(uint64_t) item = TX_ALLOC(uint64_t, sizeof(uint64_t));
  *D_RW(item) = val;
  return item;
}

int callback(const unsigned char *data,
	     const int len,
	     void **return_value)
{
  // Heartbeat
  static unsigned long tx_block_cnt   = 0;
  static unsigned long tx_block_begin = rtc_clock::current_time();
  static unsigned long total_latency = 0;

  if(rtc_clock::current_time() - tx_block_begin >= 5000000 &&
     tx_block_cnt > 0) {
    BOOST_LOG_TRIVIAL(info) << "LOAD = "
			    << ((double)1000000*tx_block_cnt)/(rtc_clock::current_time() - tx_block_begin)
			    << " tx/sec "
			    << "LATENCY ="
			    << ((double)total_latency)/tx_block_cnt
			    << " us ";
    tx_block_begin = rtc_clock::current_time();
    tx_block_cnt   = 0;
    total_latency  = 0;
  }
  tx_block_cnt++;
  unsigned long tx_begin_time = rtc_clock::current_time();
  //////////////

  struct proposal *req = (struct proposal *)data;
  int code = req->fn;
  *return_value  = malloc(sizeof(struct proposal));
  struct proposal *rep  = (struct proposal *)*return_value;
  memcpy(&rep->cookie, &req->cookie, sizeof(cookie_t));
  if(code == FN_INSERT) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->kv_data.key);
    rep->code = CODE_OK;
    rep->kv_data.key = req->kv_data.key;
    if(!OID_IS_NULL(item)) {
      pmemobj_tx_add_range(item, 0, sizeof(uint64_t));
      uint64_t *ptr = (uint64_t *)pmemobj_direct(item);
      rep->kv_data.value = *ptr;
      if(is_stable(*ptr)) {
	*ptr = req->kv_data.value;
      }
      else {
	rep->code = CODE_NOK;
      }
    }
    else {
      rbtree_map_insert(pop, 
			the_tree, 
			req->kv_data.key,
			new_store_item(req->kv_data.value).oid);
      rep->kv_data.value = req->kv_data.value;
    }
  }
  else if(code == FN_DELETE) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    rep->kv_data.key = req->kv_data.key;
    if(OID_IS_NULL(item)) {
	rep->code = CODE_NOK;
    }
    else {
      uint64_t value = *(uint64_t *)pmemobj_direct(item);
      rep->code = CODE_OK;
      rep->kv_data.value = value;
      if(is_stable(value)) {
	item = rbtree_map_remove(pop,
				 the_tree, 
				 req->k_data.key);
	pmemobj_tx_free(item);
      }
    }
  }
  else if(code == FN_LOOKUP) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    rep->kv_data.key     = req->k_data.key;
    if(OID_IS_NULL(item)) {
      rep->code = CODE_NOK;
    }
    else {
      rep->code = CODE_OK;
      rep->kv_data.value = *(uint64_t *)pmemobj_direct(item);
    }
  }
  else if(code == FN_BUMP) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    rep->kv_data.key     = req->k_data.key;
    if(OID_IS_NULL(item)) {
      rep->code = CODE_NOK;
    }
    else {
      uint64_t *ptr = (uint64_t *)pmemobj_direct(item);
      rep->kv_data.value = *ptr;
      if(is_stable(*ptr)) {
	pmemobj_tx_add_range(item, 0, sizeof(uint64_t));
	(*ptr) = (*ptr) + 2;
      }
    }
  }
  else if(code == FN_PREPARE) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    rep->code = CODE_OK;
    rep->kv_data.key     = req->k_data.key;
    if(OID_IS_NULL(item)) {
      rep->cookie.success = 0; // FAIL
    }
    else {
      pmemobj_tx_add_range(item, 0, sizeof(uint64_t));
      uint64_t *ptr = (uint64_t *)pmemobj_direct(item);
      rep->kv_data.value = *ptr;
      if(rep->kv_data.value != req->kv_data.value) {
	rep->cookie.success = 0;
      }
      (*ptr) = (*ptr) + 1;
    }
  }
  else if(code == FN_COMMIT) {
    PMEMoid item = rbtree_map_get(pop, the_tree, req->k_data.key);
    rep->code = CODE_OK;
    rep->kv_data.key     = req->k_data.key;
    if(!OID_IS_NULL(item)) {
      uint64_t *ptr = (uint64_t *)pmemobj_direct(item);
      if(!is_stable(*ptr)) {
	pmemobj_tx_add_range(item, 0, sizeof(uint64_t));
	if(req->cookie.success) {
	  (*ptr) = (*ptr) + 1;
	}
	else {
	  (*ptr) = (*ptr) - 1;
	}
      }
    }
  }
  else {
    BOOST_LOG_TRIVIAL(fatal) << "Tree: unknown fn !";
    exit(-1);
  }
  total_latency += (rtc_clock::current_time() - tx_begin_time);
  return sizeof(struct proposal);
}

TOID(char) nvheap_setup(TOID(char) recovered,
			PMEMobjpool *state)
{
  
  TOID(char) store;
  heap_root_t *heap_root;
  pop = state;
  if(TOID_IS_NULL(recovered)) {
    store = TX_ALLOC(char, sizeof(heap_root_t));
    heap_root = (heap_root_t *)D_RW(store);
    pmemobj_tx_add_range_direct(heap_root, sizeof(heap_root_t));
    rbtree_map_new(state, &heap_root->the_tree, NULL);
    the_tree = heap_root->the_tree;
    return store;
  }
  else {
    heap_root = (heap_root_t *)D_RW(recovered);
    the_tree = heap_root->the_tree;
    return recovered;
  }
}

void gc(void *data)
{
  free(data);
}



int main(int argc, char *argv[])
{
  if(argc != 4 && argc != 6) {
    printf("Usage1: %s server_id replicas clients \n", argv[0]);
    printf("Usage1: %s server_id replicas clients server_config client_config\n", argv[0]);
    exit(-1);
  }
  server_id = atoi(argv[1]);
  int replicas = atoi(argv[2]);
  int clients  = atoi(argv[3]);
  if(argc == 4) {
    dispatcher_start("cyclone_test.ini", "cyclone_test.ini", callback, 
		     NULL, NULL, 
		     gc, nvheap_setup, server_id, replicas, clients);
  }
  else {
      dispatcher_start(argv[4], argv[5], callback, 
		       NULL, NULL, 
		       gc, nvheap_setup, server_id, replicas, clients);
  }
}


