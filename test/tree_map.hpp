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

const int FN_INSERT = 0;
const int FN_DELETE = 1;
const int FN_LOOKUP = 2;
const int FN_BUMP   = 3;
const int FN_LOCK   = 4;
const int FN_GET_VERSION = 5;
const int FN_UNLOCK = 6;

const int CODE_OK   = 0;
const int CODE_NOK  = 1;

struct k {uint64_t key;};
struct kv {uint64_t key; uint64_t value;};


typedef struct cookie_st {
  int phase;
  int locks_taken;
  int success;
  int index;
} cookie_t;

const int COOKIE_PHASE_LOCK     = 0;
const int COOKIE_PHASE_VERSION  = 1;
const int COOKIE_PHASE_INSERT   = 2;
const int COOKIE_PHASE_DELETE   = 3;
const int COOKIE_PHASE_UNLOCK   = 4;

struct proposal {
  union {
    int fn;
    int code;
  };
  unsigned long timestamp;
  unsigned long src;
  unsigned long order;
  union {
    struct kv kv_data;
    struct k k_data;
  };
  cookie_t cookie;
};


#endif /* TREE_MAP_H */
