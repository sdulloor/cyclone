#include<signal.h>
#include<sys/mman.h>
#include<string.h>
#include "logging.h"

static struct sigaction orig_sigsegv_handler;
static void *mapping;
static unsigned long mapping_size;
static save_page_t *saved_pages = NULL;
static const int pagesize = 4096;

static void sigsegv_handler(int sig, siginfo_t *siginfo, void *context)
{
  save_page_t * check = saved_pages;
  void *page = siginfo->si_addr;
  while(check != NULL) {
    if(check->page_address == page)
      return;
    check = check->next;
  }
  check = (save_page_t *)malloc(sizeof(save_page_t));
  if(check == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Savepage out of memory";
    exit(-1);
  }
  check->page_address  = page;
  check->saved_version = malloc(pagesize);
  if(check->saved_version == NULL) {
    BOOST_LOG_TRIVIAL(fatal) << "Savepage out of memory";
    exit(-1);
  }
  memcpy(check->saved_version, check->page_address, pagesize);
  check->next = saved_pages;
  saved_pages = check;
  // Unprotect the page
  int e = mprotect(page, pagesize, PROT_READ|PROT_WRITE);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to un write-protect heap page in sighandler"
      << strerror(errno);
  }
}
 
void init_sigsegv_handler(void *mapping_in,
			  unsigned long mapping_size_in)
{
  struct sigaction sigsegv;
  mapping      = mapping_in;
  mapping_size = mapping_size_in; 
  sigsegv.sa_sigaction = sigsegv_handler;
  sigemptyset(&sigsegv.sa_mask);
  sigsegv.sa_flags = SA_SIGINFO;
  sigaction(SIGSEGV, &sigsegv, &orig_sigsegv_handler);
  // write protect heap here with the handler in place
  int e = mprotect(mapping, mapping_size, PROT_READ);
  if(e < 0) {
    BOOST_LOG_TRIVIAL(fatal)
      << "Failed to write-protect heap"
      << strerror(errno);
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
  sigaction(SIGSEGV, &orig_sigsegv_handler, NULL);
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
