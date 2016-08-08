#ifndef _CLWBOPTSIM_
#define _CLWBOPTSIM_


static const int CLFLUSHOPT_WINDOW = 10;

static void clflush(void *ptr, int size)
{
  char *x = (char *)ptr;
  while(size > 64*CLFLUSHOPT_WINDOW) {
    asm volatile("clflush %0"::"m"(*x));
    x    += 64*CLFLUSHOPT_WINDOW;
    size -= 64*CLFLUSHOPT_WINDOW;
  }
  asm volatile("clflush %0"::"m"(*x));
}

static int clflush_partial(void *ptr, int size, int clflush_cnt)
{
  char *x = (char *)ptr;
  while(size > 64) {
    clflush_cnt++;
    if(clflush_cnt == CLFLUSHOPT_WINDOW) {
      asm volatile("clflush %0"::"m"(*x));
      clflush_cnt = 0;
    }
    x    += 64;
    size -= 64;
  }
  clflush_cnt++;
  if(clflush_cnt == CLFLUSHOPT_WINDOW) {
    asm volatile("clflush %0"::"m"(*x));
    clflush_cnt = 0;
  }
  return clflush_cnt;
}


#endif
