#ifndef _CIRCULAR_LOG_
#define _CIRCULAR_LOG_
#include<libpmemobj.h>
#include "pmem_layout.h"
void copy_from_circular_log(const struct circular_log *log,
			    unsigned long LOGSIZE,
			    unsigned char *dst,
			    unsigned long offset,
			    unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  memcpy(dst, log->data + offset, chunk1);
  if(chunk2 > 0) {
    dst += chunk1;
    memcpy(dst, log->data, chunk2);
  }
}

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
  memcpy(log->data + offset, src, chunk1);
  if(chunk2 > 0) {
    src += chunk1;
    memcpy(log->data, src, chunk2);
  }
}

void persist_to_circular_log(PMEMobjpool *pop, 
			     struct circular_log *log,
			     unsigned long LOGSIZE,
			     unsigned long offset,
			     unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  pmemobj_persist(pop, log->data + offset, chunk1);
  if(chunk2 > 0) {
    pmemobj_persist(pop, log->data, chunk2);
  }
}

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
