#ifndef _CIRCULAR_LOG_
#define _CIRCULAR_LOG_
#include<libpmemobj.h>
#include "pmem_layout.h"

static const char * log_data(const struct circular_log *log)
{
  return (const char *)(log + 1);
}

static char * log_data(struct circular_log *log)
{
  return (char *)(log + 1);
}

static
void copy_from_circular_log(const struct circular_log *log,
			    unsigned long LOGSIZE,
			    unsigned char *dst,
			    unsigned long offset,
			    unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  memcpy(dst, log_data(log) + offset, chunk1);
  if(chunk2 > 0) {
    dst += chunk1;
    memcpy(dst, log_data(log), chunk2);
  }
}

static
void copy_to_circular_log(PMEMobjpool *pop, 
			  struct circular_log* log,
			  unsigned long LOGSIZE,
			  unsigned long offset,
			  unsigned char *src,
			  unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  memcpy(log_data(log) + offset, src, chunk1);
  if(chunk2 > 0) {
    src += chunk1;
    memcpy(log_data(log), src, chunk2);
  }
}

static void clflush(void *ptr, int size)
{
  return;
  char *x = (char *)ptr;
  while(size > 64) {
    asm volatile("clflush %0"::"m"(*x));
    x += 64;
    size -=64;
  }
  asm volatile("clflush %0;mfence"::"m"(*x));
}




static void persist_to_circular_log(PMEMobjpool *pop, 
			     struct circular_log *log,
			     unsigned long LOGSIZE,
			     unsigned long offset,
			     unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  clflush(log_data(log) + offset, chunk1);
  if(chunk2 > 0) {
    clflush(log_data(log), chunk2);
  }
}

static
unsigned long circular_log_advance_ptr(unsigned long ptr,
				       unsigned long size,
				       unsigned long LOGSIZE)
{
  ptr = ptr + size;
  if(ptr > LOGSIZE) {
    ptr = ptr - LOGSIZE;
  }
  return ptr;
}

static
unsigned long circular_log_recede_ptr(unsigned long ptr,
				      unsigned long size,
				      unsigned long LOGSIZE)
{
  if(ptr < size) {
    ptr = LOGSIZE - (size - ptr);
  }
  else {
    ptr = ptr - size;
  }
  return ptr;
}

#endif
