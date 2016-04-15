#ifndef _CHECKPOINT_
#define _CHECKPOINT_
void init_checkpoint(const char *fname);
const char* get_checkpoint_fname();
void take_checkpoint(int leader_term,
		     int raft_idx,
		     int raft_term);
void send_checkpoint(void *socket);
void build_image(void *socket);
void delete_checkpoint(void *checkpoint);

typedef struct fragment_st {
  int term;
  int last_included_index;
  int last_included_term;
  unsigned long offset;
}fragment_t;

#define REPLY_OK 0UL
#define REPLY_STALE -1UL

#endif
