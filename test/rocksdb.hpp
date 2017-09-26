#ifndef _ROCKSDB_COMMON_
#define _ROCKSDB_COMMON_
const unsigned long OP_PUT       = 0;
const unsigned long OP_GET       = 1;
const unsigned long OP_ADD       = 2;
const unsigned long value_sz     = 8;
typedef struct rock_kv_st{
  unsigned long op;
  unsigned long key;
  char value[value_sz];
}rock_kv_t;
typedef struct rock_kv_pair_st {
  unsigned long key;
  char value[value_sz];
}rock_kv_pair_t;
const char *preload_dir = "/dev/shm/preloaded";
const char *data_dir = "/dev/shm/checkpoint";
const char *log_dir  = "/mnt/ssd/logs";
const unsigned long rocks_keys = 100000000;
const int use_flashlog   = 1;
const int use_rocksdbwal = 0;
#endif
