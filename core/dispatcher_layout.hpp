#ifndef _DISPATCHER_LAYOUT_
#define _DISPATCHER_LAYOUT_
#include<libpmemobj.h>
#include "libcyclone.hpp"
POBJ_LAYOUT_BEGIN(disp_state);
typedef struct disp_state_st {
} disp_state_t;
TOID_DECLARE_ROOT(disp_state_t);
POBJ_LAYOUT_END(disp_state);
#endif
