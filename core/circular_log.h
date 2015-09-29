#ifndef _CIRCULAR_LOG_
#define _CIRCULAR_LOG_
struct circular_log
{
  unsigned long log_head;
  unsigned long log_tail;
  unsigned char data[0];
};

void copy_from_circular_log(TOID(struct circular log) log,
			    unsigned long LOGSIZE,
			    unsigned char *dst,
			    unsigned long offset,
			    unsigned long size)
{
  unsigned long chunk1 = (offset + size) > LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  TX_MEMCPY(dst, D_RO(log)->data + offset, chunk1);
  if(chunk2 > 0) {
    dst += chunk1;
    TX_MEMCPY(dst, D_RO(log)->data, chunk2);
  }
}

void copy_to_circular_log(TOID(struct circular log) log,
			  unsigned long LOGSIZE,
			  unsigned long offset,
			  unsigned char *src,
			  unsigned long size)
{
  unsigned long chunk1 = (offset + size) > RAFT_LOGSIZE ?
    (LOGSIZE - offset):size;
  unsigned long chunk2 = size - chunk1;
  TX_MEMCPY(D_RW(log)->data + offset, src, chunk1);
  if(chunk2 > 0) {
    src += chunk1;
    TX_MEMCPY(D_RW(log)->data, src, chunk2);
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
