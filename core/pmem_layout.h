#ifndef _PMEM_LAYOUT_
#define _PMEM_LAYOUT_
struct circular_log
{
  unsigned long log_head;
  unsigned long log_tail;
};
POBJ_LAYOUT_BEGIN(raft_persistent_state);
POBJ_LAYOUT_TOID(raft_persistent_state, struct circular_log)
typedef struct raft_pstate_st {
  int term;
  int voted_for;
  TOID(struct circular_log) log;
} raft_pstate_t;
POBJ_LAYOUT_ROOT(raft_persistent_state, raft_pstate_t);
POBJ_LAYOUT_END(raft_persistent_state);

typedef TOID(struct circular_log) log_t;

#endif
