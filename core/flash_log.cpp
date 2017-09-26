#include<stdlib.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<fcntl.h>
#include<libaio.h>
#include "logging.hpp"
#include "libcyclone.hpp"
typedef struct log_page_st {
  struct iocb cb;
  char *page;
} log_page_t;


typedef struct flash_log_st {
  int log_fd;
  log_page_t log_pages[2];
  int active_page;
  int bytes_on_active_page;
  int entries_on_active_page;
  int raft_idx;
  int checkpointed_raft_idx;
  int inflight_raft_idx;
  bool issued_io;
  int issued_bytes;
  io_context_t ctx;
  unsigned long logsize;
  unsigned long max_logsize;
} flash_log_t;

static void log_switch_page(flash_log_t *log)
{
  struct iocb *ios[1];
  struct io_event events[1];
  int e;
  if(log->issued_io) {
    log_page_t * inflight_page = &log->log_pages[1 - log->active_page];
    e = 0;
    while(e <= 0) {
      e = io_getevents(log->ctx, 1, 1, events, NULL);
    }
    if(events[0].res != log->issued_bytes) {
      BOOST_LOG_TRIVIAL(fatal) << "Async IO reported failure:"
			       << (int)events[0].res;
      exit(-1);
    }
    log->checkpointed_raft_idx = log->inflight_raft_idx - 1;
  }
  if(log->logsize >= log->max_logsize) {
    if(e = posix_fallocate(log->log_fd, log->logsize, flashlog_segsize)) {
      BOOST_LOG_TRIVIAL(fatal) << "preallocation failed: " << e;
      exit(-1);
    }
    log->max_logsize += flashlog_segsize;
  }
  log_page_t * issue_page = &log->log_pages[log->active_page];
  *(unsigned long *)issue_page->page = log->bytes_on_active_page;
  memset(&issue_page->cb, 0, sizeof(struct iocb));
  issue_page->cb.aio_lio_opcode = IO_CMD_PWRITE;
  issue_page->cb.aio_fildes      = log->log_fd;
  issue_page->cb.u.c.buf         = issue_page->page;
  log->issued_bytes              = ((log->bytes_on_active_page + 4095)/4096)*4096;
  log->logsize                   += log->issued_bytes;
  issue_page->cb.u.c.nbytes      = log->issued_bytes;
  issue_page->cb.u.c.offset      = 0;
  ios[0] = &issue_page->cb;
  e = io_submit(log->ctx, 1, ios);
  if(e < 1) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to submit asynchronous IO: "
			     << e;
    BOOST_LOG_TRIVIAL(info) << "Flashlog fd = " << log->log_fd;
    exit(-1);
  }
  log->active_page = 1 - log->active_page;
  log->issued_io = true;
  log->inflight_raft_idx = log->raft_idx;
  log->bytes_on_active_page = sizeof(unsigned long);
  log->entries_on_active_page = 0;
}

void *create_flash_log(const char *path)
{
  int e;
  int fd = open(path, 
		O_WRONLY|O_APPEND|O_TRUNC|O_CREAT|O_DIRECT|(flashlog_use_osync ? O_SYNC:0), 
		0644);
  if(fd == -1) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to create flash log";
    exit(-1);
  }
  BOOST_LOG_TRIVIAL(info) << "Preallocating flashlog segment";
  if(e = posix_fallocate(fd, 0, flashlog_segsize)) {
    BOOST_LOG_TRIVIAL(fatal) << "preallocation failed: " << e;
    exit(-1);
  }
  BOOST_LOG_TRIVIAL(info) << "Done preallocating flashlog segment";
  flash_log_t *log = (flash_log_t *)malloc(sizeof(flash_log_t));
  log->log_fd = fd;
  BOOST_LOG_TRIVIAL(info) << "Flashlog fd = " << fd;
  log->ctx = 0;
  if((e = io_setup(100, &log->ctx)) != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to setup aio context:"
			     << e;
    exit(-1);
  }
  log->issued_io = false;
  log->logsize   = 0;
  log->max_logsize = flashlog_segsize;
  log->active_page = 0;
  if(posix_memalign((void **)&log->log_pages[0].page,
		    4096, 
		    flashlog_pagesize) != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to allocate aligned page";
    exit(-1);
  }
  
  memset(&log->log_pages[0].cb, 0, sizeof(iocb));
  if(posix_memalign((void **)&log->log_pages[1].page,
		    4096, 
		    flashlog_pagesize) != 0) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to allocate aligned page";
    exit(-1);
  }
  memset(&log->log_pages[1].cb, 0, sizeof(iocb));
  log->raft_idx = -1;
  log->checkpointed_raft_idx = -1;
  log->bytes_on_active_page = sizeof(unsigned long);
  log->entries_on_active_page = 0;
  return (void *)log;
}

int log_append(void *log_, 
	       const char *data, 
	       int size,
	       int raft_idx)
{
  flash_log_t *log = (flash_log_t *)log_;
  int bytes_left = flashlog_pagesize - log->bytes_on_active_page;
  if(bytes_left < (size + sizeof(unsigned long))) {
    log_switch_page(log);
  }
  else if(log->entries_on_active_page >= flashlog_hwm) {
    log_switch_page(log);
  }
  unsigned long stop_byte = log->bytes_on_active_page + size - 1;
  // Skip to next page if necessary
  if(stop_byte/4096 != log->bytes_on_active_page/4096) {
    if(bytes_left < 4096) {
      log_switch_page(log);
    }
    else {
      log->bytes_on_active_page = 
	((log->bytes_on_active_page + 4096)/4096)*4096;
    }
  }
  char *buffer = log->log_pages[log->active_page].page;
  buffer = buffer + log->bytes_on_active_page;
  *(unsigned long *)buffer = size;
  buffer = buffer + sizeof(unsigned long);
  memcpy(buffer, data, size);
  log->bytes_on_active_page += (size + sizeof(unsigned long));
  log->entries_on_active_page++;
  log->raft_idx = raft_idx;
  return log->checkpointed_raft_idx;
}


