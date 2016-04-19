#ifndef _CHECKPOINT_
#define _CHECKPOINT_
void init_checkpoint(const char *fname, int master);
const char* get_checkpoint_fname();
void take_checkpoint(int leader_term,
		     int raft_idx,
		     int raft_term);
void send_checkpoint(void *socket, void *cyclone_handle);
void init_build_image(void *socket,
		      int *termp,
		      int* indexp,
		      int *masterp,
		      void **init_ety_ptr);
void build_image(void *socket);
int image_get_term();
int image_get_idx();
void delete_checkpoint(void *checkpoint);

typedef struct fragment_st {
  int master;
  int term;
  int last_included_index;
  int last_included_term;
  unsigned long offset;
}fragment_t;

#define REPLY_OK 0UL
#define REPLY_STALE -1UL

typedef struct savepage_st{
  unsigned long offset; // in backing file
  void *saved_version;
  struct savepage_st *next;
} save_page_t;

extern save_page_t *saved_pages;

extern void init_sigsegv_handler(const char *fname);
extern void restore_sigsegv_handler();
extern void delete_saved_pages();

static const unsigned long pagesize = 4096;


#endif
