#ifndef _ROCKSDB_COMMON_
#define _ROCKSDB_COMMON_
const unsigned long OP_PUT = 0;
const unsigned long OP_GET = 1;
typedef struct rock_kv_st{
  unsigned long op;
  unsigned long key;
  char value[256];
}rock_kv_t;
#endif
