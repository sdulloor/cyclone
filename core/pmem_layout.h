#ifndef _PMEM_LAYOUT_
#define _PMEM_LAYOUT_
struct circular_log
{
  volatile int head;
  volatile int tail;
};
typedef struct raft_pstate_st {
  int term;
  int voted_for;
  struct circular_log log;
} __attribute__((packed)) raft_pstate_t;
#endif
