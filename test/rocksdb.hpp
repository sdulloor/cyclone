#ifndef _ROCKSDB_COMMON_
#define _ROCKSDB_COMMON_
const unsigned long OP_PUT   = 0;
const unsigned long OP_GET   = 1;
const unsigned long value_sz = 8;
typedef struct rock_kv_st{
  unsigned long op;
  unsigned long key;
  char value[value_sz];
}rock_kv_t;
const char *preload_dir = "/mnt/ssd/preloaded";
const char *data_dir = "/mnt/ssd/checkpoint";
const char *log_dir  = "/mnt/ssd/checkpoint";
const unsigned long rocks_keys = 100000000;
const int use_flashlog   = 1;
const int use_rocksdbwal = 1;
#endif
