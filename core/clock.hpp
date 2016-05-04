#ifndef _CLOCK_
#define _CLOCK_
#include<sys/time.h>
#include "logging.hpp"

//! Real time clock
class rtc_clock {
  const char *msg;
  unsigned long dump_interval;
  unsigned long accumulator;
  unsigned long samples;
  static unsigned long get_current_rtc()
  {
    struct timeval tm;
    gettimeofday(&tm, NULL);
    return tm.tv_sec*1000000 + tm.tv_usec;
  }

 public:
  rtc_clock(const char *msg_in, 
	    unsigned long dump_interval_in)
    :msg(msg_in),
     dump_interval(dump_interval_in)
  {
    accumulator = 0;
    samples = 0;
  }
  static unsigned long current_time()
  {
    return get_current_rtc();
  }
  void sample(unsigned long sample)
  {
    accumulator += sample;
    samples++;
    if(samples >= dump_interval) {
      BOOST_LOG_TRIVIAL(info) << msg
			      << ((double)accumulator/samples);
    }
  }
};

#endif
