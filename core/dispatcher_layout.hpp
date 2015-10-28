#ifndef _DISPATCHER_LAYOUT_
#define _DISPATCHER_LAYOUT_
#include<libpmemobj.h>
POBJ_LAYOUT_BEGIN(disp_state);
POBJ_LAYOUT_TOID(disp_state, char);
struct client_state_st {
  unsigned long committed_txid;
  TOID(char) last_return_value;
  int last_return_size;
};
typedef struct disp_state_st {
  struct client_state_st client_state[MAX_CLIENTS];
} disp_state_t;
POBJ_LAYOUT_ROOT(disp_state, disp_state_t);
POBJ_LAYOUT_END(disp_state);
#endif
