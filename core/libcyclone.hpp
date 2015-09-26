#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_

int cyclone_is_leader(); // returns 1 if true

typedef struct cyclone_req_st {
  unsigned char *data;
  int size;
  volatile int response_code; // 1 = success, -1 = fail
  volatile int request_complete; // set to 1 when complete
} cyclone_req_t;


extern int cyclone_add_entry(cyclone_req_t *req); // returns 0 if success

typedef void (*cyclone_callback_t)(const unsigned char *data, const int len);
extern void cyclone_boot(const char *config_path,
			 cyclone_callback_t cyclone_callback);

extern void cyclone_shutdown();
#endif
