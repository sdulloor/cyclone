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
  void *page_address;
  void *saved_version;
  struct savepage_st *next;
} save_page_t;

extern save_page_t *saved_pages;

void init_sigsegv_handler(void *mapping_in,
			  unsigned long mapping_size_in);
void restore_sigsegv_handler();

#endif
