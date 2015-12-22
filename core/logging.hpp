#ifndef _LOGGING_
#define _LOGGING_
#include<iostream>
#include<string>
#include<sstream>
#include<boost/thread.hpp>
#include<boost/date_time/posix_time/posix_time.hpp>
enum log_state {
  fatal = 0,
  error = 1,
  warning = 2,
  info = 3,
  total = 4
};

static const char *log_headers[total] = 
  {"FATAL", 
   "ERROR",
   "WARNING",
   "INFO"};

class BOOST_LOG_TRIVIAL {
  enum log_state level;
public:
  std::stringstream state;
  BOOST_LOG_TRIVIAL(enum log_state level_in)
    :level(level_in)
  {
  }
  template<typename T>
  BOOST_LOG_TRIVIAL& operator<< (T value)
  {
    state << value;
    return *this;
  }
  
  ~BOOST_LOG_TRIVIAL()
  {
    std::stringstream final;
    final << "<" << log_headers[level] << " ";
    final << boost::posix_time::to_simple_string(boost::posix_time::microsec_clock::local_time()) << " ";
    final << boost::this_thread::get_id() << "> ";
    final << state.str() << std::endl;
    std::cerr << final.str();
  }
};

#endif
