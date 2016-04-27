#include<signal.h>
#include<sys/mman.h>
#include<string.h>
#include<stdio.h>
#include "checkpoint.hpp"
#include "logging.hpp"

static struct sigaction orig_sigsegv_handler;
static void *mapping;
static unsigned long mapping_size;
static unsigned long map_offset;
save_page_t *saved_pages = NULL;

static void check_sigsegv_range(unsigned long addr)
{
  unsigned long base = (unsigned long)mapping;
  if(addr < base || addr >= (base + mapping_size)) {
    BOOST_LOG_TRIVIAL(fatal) << "SIGSEGV from app outside monitored range";
    exit(-1);
  }
}

static void sigsegv_handler(int sig, siginfo_t *siginfo, void *context)
{
  save_page_t * check = saved_pages;
  void *page = siginfo->si_addr;
  check_sigsegv_range((unsigned long)page);
  void *page_aligned = (void *)((unsigned long)page & ~ (pagesize - 1));
  unsigned long offset = map_offset + ((char *)page_aligned - (char *)mapping);
  while(check != NULL) {
    if(check->offset == offset)
      return;
    check = check->next;
  }
  check = (save_page_t *)malloc(sizeof(save_page_t));
  if(check == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Savepage out of memory";
    exit(-1);
  }
  check->offset  = offset;
  check->saved_version = malloc(pagesize);
  if(check->saved_version == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Savepage out of memory";
    exit(-1);
  }
  memcpy(check->saved_version, 
	 page_aligned, 
	 pagesize);
  check->next = saved_pages;
  saved_pages = check;
  // Unprotect the page
  int e = mprotect(page_aligned, pagesize, PROT_READ|PROT_WRITE);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to un write-protect heap page in sighandler"
      << strerror(errno);
    exit(-1);
  }
}
 
void init_sigsegv_handler(const char *fname)
{
  struct sigaction sigsegv;
  /* Parse /proc/self/maps to detect mapping */
  FILE *fp = fopen("/proc/self/maps", "r");
  if(fp == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to open proc/self/maps for reading:"
			     << strerror(errno);
    exit(-1);
  }
  char *buffer = (char *)malloc(1000);
  while(true) {
    size_t size = 1000;
    int readbytes = getline(&buffer, &size, fp);
    if(readbytes == -1) {
      break;
    }
    int ptr = strlen(buffer);
    unsigned long map_begin, map_end, offset, ino;
    int major, minor;
    char p1,p2,p3,p4;
    int t = sscanf(buffer, "%lx-%lx %c%c%c%c %llx %x:%x %lu",
		   &map_begin, &map_end, &p1, &p2, &p3, &p4, &offset, 
		   &major, &minor, &ino);
    if(t != 10) {
      BOOST_LOG_TRIVIAL(fatal) 
	<< "Failed to parse maps line:"
	<< (const char *)buffer;
    }
    while(buffer[--ptr] != ' ') {
      if(buffer[ptr] == '\n') {
	buffer[ptr] = 0;
      }
    }
    char *name = &buffer[++ptr];
    if(strcmp(name, fname) == 0 && p2 == 'w') {
      mapping = (void *)map_begin;
      mapping_size = (map_end - map_begin);
      map_offset = offset;
      break;
    }
  }
  free(buffer);
  fclose(fp);
  sigsegv.sa_sigaction = sigsegv_handler;
  sigemptyset(&sigsegv.sa_mask);
  sigsegv.sa_flags = SA_SIGINFO;
  int e = sigaction(SIGSEGV, &sigsegv, &orig_sigsegv_handler);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to install sigsegv handler"
      << strerror(errno);
    exit(-1);
  }
  // write protect heap here with the handler in place
  e = mprotect(mapping, mapping_size, PROT_READ);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to write-protect heap"
      << strerror(errno);
    exit(-1);
  }
}

void restore_sigsegv_handler()
{
  // un-write protect the heap
  int e = mprotect(mapping, mapping_size, PROT_READ|PROT_WRITE);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to un write-protect heap"
      << strerror(errno);
  }
  // Re-install the original signal handler
  e = sigaction(SIGSEGV, &orig_sigsegv_handler, NULL);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to restore sigsegv handler"
      << strerror(errno);
    exit(-1);
  }
}

void delete_saved_pages()
{
  // Free the saved pages structure
  while(saved_pages != NULL) {
    save_page_t *page = saved_pages;
    saved_pages = saved_pages->next;
    free(page->saved_version);
    free(page);
  }
}
