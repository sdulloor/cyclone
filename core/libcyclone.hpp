#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_

int cyclone_is_leader(void *cyclone_handle); // returns 1 if true

// Returns a non-null cookie if accepted for replication
extern void *cyclone_add_entry(void * cyclone_handle, void *data, int size); 
// Returns 0:pending 1:success -1:failed
extern int cyclone_check_status(void *cyclone_handle, void *cookie);
// Callback to apply a log entry
typedef void (*cyclone_callback_t)(void *user_arg,
				   const unsigned char *data,
				   const int len);
// Returns a cyclone handle
extern void* cyclone_boot(const char *config_path,
			  cyclone_callback_t cyclone_callback,
			  void *user_arg);

extern void cyclone_shutdown(void *cyclone_handle);
#endif
