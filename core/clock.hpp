#ifndef _CLOCK_
#define _CLOCK_
#include<sys/time.h>
#include "logging.hpp"

//! Real time clock
class rtc_clock {
  unsigned long start_time;
  unsigned long elapsed_useconds;
  const char *msg;
  static unsigned long get_current_rtc()
  {
    struct timeval tm;
    gettimeofday(&tm, NULL);
    return tm.tv_sec*1000000 + tm.tv_usec;
  }

 public:
 rtc_clock(const char *msg_in)
   :msg(msg_in)
  {
  }
  static unsigned long current_time()
  {
    return get_current_rtc();
  }
};

#endif
