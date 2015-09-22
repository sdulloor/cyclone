#ifndef _CYCLONE_HPP_
#define _CYCLONE_HPP_

int cyclone_is_leader();
int cyclone_add_entry(unsigned char *data, const int size);
int cyclone_req_complete();
typedef void (*cyclone_callback_t)(const unsigned char *data, const int len);
extern void cyclone_boot(char *config_path,
			 cyclone_callback_t cyclone_callback);

#endif
