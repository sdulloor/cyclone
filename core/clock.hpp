#ifndef _CLOCK_
#define _CLOCK_
#include<sys/time.h>
#include "logging.hpp"

//! Real time clock
class rtc_clock {
  unsigned long start_time;
  unsigned long elapsed_useconds;
  unsigned long get_current_rtc()
  {
    struct timeval tm;
    gettimeofday(&tm, NULL);
    return tm.tv_sec*1000000 + tm.tv_usec;
  }

 public:
 rtc_clock()
   :elapsed_useconds(0)
    {
    }
  void start()
  {
    start_time = get_current_rtc();
  }
  void stop()
  {
    elapsed_useconds += (get_current_rtc() - start_time);
  }
  void reset()
  {
    elapsed_useconds = 0;
  }
  unsigned long elapsed_time()
  {
    return elapsed_useconds;
  }
  unsigned long current_time()
  {
    return get_current_rtc();
  }
  void print(const char header[])
  {
    double elapsed_seconds = (double)elapsed_useconds/1000000;
    BOOST_LOG_TRIVIAL(info) << header << " " << elapsed_seconds << " seconds";
  }
};

#endif
