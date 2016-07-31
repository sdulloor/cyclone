#ifndef _CLWBOPTSIM_
#define _CLWBOPTSIM_


static const int CLFLUSHOPT_WINDOW = 10;

static void clflush(void *ptr, int size)
{
  char *x = (char *)ptr;
  asm volatile("mfence");
  while(size > 64*CLFLUSHOPT_WINDOW) {
    asm volatile("clflush %0"::"m"(*x));
    x    += 64*CLFLUSHOPT_WINDOW;
    size -= 64*CLFLUSHOPT_WINDOW;
  }
  asm volatile("clflush %0;mfence"::"m"(*x));
}


#endif
