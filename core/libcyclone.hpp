#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_

int cyclone_is_leader(); // returns 1 if true

// Returns a non-null cookie if accepted for replication
extern void *cyclone_add_entry(void *data, int size); 
// Returns 0:pending 1:success -1:failed
extern int cyclone_check_status(void *cookie);
typedef void (*cyclone_callback_t)(const unsigned char *data, const int len);
extern void cyclone_boot(const char *config_path,
			 cyclone_callback_t cyclone_callback);

extern void cyclone_shutdown();
#endif
