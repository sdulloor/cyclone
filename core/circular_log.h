#ifndef _CIRCULAR_LOG_
#define _CIRCULAR_LOG_
#include<libpmemobj.h>
#include "pmem_layout.h"
#include "clwb_sim.hpp"

static void **log_data(struct circular_log *log)
{
  return (void **)(log + 1);
}

// Return -1 if log full
static int log_offer(struct circular_log *log,
		     void *ptr,
		     int tail,
		     int LOG_ENTRIES)
{
  log_data(log)[tail++] = ptr;
  if(tail == LOG_ENTRIES)
    tail = 0;
  if(tail == log->head)
    return -1;
  else
    return tail;
}

static void log_poll(struct circular_log *log,
		     int LOG_ENTRIES)
{
  int new_head = log->head + 1;
  if(new_head == LOG_ENTRIES)
    new_head = 0;
  log->head = new_head;
  asm volatile("mfence;clflush %0"::"m"(*log));
}

static void log_poll_batch(struct circular_log *log,
			   int cnt,
			   int LOG_ENTRIES)
{
  int new_head = log->head + cnt;
  if(new_head >= LOG_ENTRIES)
    new_head = new_head - LOG_ENTRIES;
  log->head = new_head;
  asm volatile("mfence;clflush %0"::"m"(*log));
}

static void log_pop(struct circular_log *log,
		    unsigned long LOG_ENTRIES)
{
  int new_tail;
  if(log->tail == 0)
    new_tail = LOG_ENTRIES - 1;
  else
    new_tail = log->tail - 1;
  log->tail = new_tail;
  asm volatile("mfence;clflush %0"::"m"(*log));
}

static void log_persist(struct circular_log *log,
			int new_tail,
			unsigned long LOG_ENTRIES)
{
  asm volatile("mfence");
  int old_tail = log->tail;
  int blocks = 0;
  asm volatile("clflush %0"::"m"(log_data(log)[old_tail]));
  while(old_tail != new_tail) {
    old_tail = old_tail + 1;
    blocks++;
    if(blocks == 8) {
      asm volatile("clflush %0"::"m"(log_data(log)[old_tail]));
      blocks = 0;
    }
    if(old_tail == LOG_ENTRIES) {
      old_tail = 0;
      asm volatile("clflush %0"::"m"(log_data(log)[old_tail]));
      blocks = 0;
    }
  }
  log->tail = new_tail;
  asm volatile("clflush %0"::"m"(*log));
}

#endif
