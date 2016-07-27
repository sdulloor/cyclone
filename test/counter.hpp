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

/*
 * tree_map.c -- TreeMap sorted collection implementation
 */

#ifndef	TREE_MAP_HPP
#define	TREE_MAP_HPP

#include <libpmemobj.h>
#include <libcyclone.hpp>

const int FN_INSERT     = 0;
const int FN_DELETE     = 1;
const int FN_LOOKUP     = 2;
const int FN_BUMP       = 3;
const int FN_PREPARE    = 4;
const int FN_COMMIT     = 5;
const int FN_SET_SLEEP  = 6; // Performance testing
const int FN_NOOP       = 7; //
const int FN_NOOP_RO    = 8;

const int CODE_OK       = 0;
const int CODE_NOK      = 1;

struct k {uint64_t key;};
struct kv {uint64_t key; uint64_t value;};


typedef struct cookie_st {
  int txnum;
  int index;
  int success;
} cookie_t;


struct proposal {
  union {
    int fn;
    int code;
  };
  union {
    struct kv kv_data;
    struct k k_data;
  };
  cookie_t cookie;
};

static uint64_t get_key(struct proposal *p)
{
  if(p->fn == FN_INSERT) {
    return p->kv_data.key;
  }
  else if(p->fn == FN_DELETE) {
    return p->k_data.key;
  }
  else if(p->fn == FN_LOOKUP) {
    return p->k_data.key;
  }
  else if(p->fn == FN_BUMP) {
    return p->k_data.key; 
  }
  else if(p->fn == FN_PREPARE) {
    return p->kv_data.key;
  }
  else if(p->fn == FN_COMMIT) {
    return p->k_data.key;
  }
  return 0; // Unknown !
}

static bool is_stable(uint64_t value)
{
  return (value %2 == 0);
}

typedef struct rbtree_tx_st {
  int steps;
} rbtree_tx_t; // Followed by steps*proposal_t 

#endif /* TREE_MAP_H */
