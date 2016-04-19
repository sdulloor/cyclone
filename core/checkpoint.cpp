#include<sys/mman.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/fcntl.h>
#include<string.h>
#include<stdlib.h>
#include "checkpoint.hpp"
#include "logging.hpp"
#include "cyclone_comm.hpp"
#include "cyclone.hpp"
static char fname[500];
static fragment_t checkpoint_hdr;
const int bufbytes = 4*1024*1024;
static void *buffer;
static unsigned long checkpoint_size;

void init_checkpoint(const char *fname_in, int master)
{
  strncpy(fname, fname_in, 50);
  buffer = malloc(bufbytes);
  checkpoint_hdr.master = master;
}

const char* get_checkpoint_fname()
{
  return fname;
}

void take_checkpoint(int leader_term,
		     int raft_idx,
		     int raft_term)
{
  checkpoint_hdr.term = leader_term;
  checkpoint_hdr.last_included_index = raft_idx;
  checkpoint_hdr.last_included_term = raft_term;
  checkpoint_size = 0;
  // Prep save pages handler
  init_sigsegv_handler(fname);
  // Return immediately
}

void send_checkpoint(void *socket, void *cyclone_handle)
{
  uint64_t reply;
  /* First send the header to start up the other side */
  int bytes_to_send = sizeof(fragment_t);
  checkpoint_hdr.offset = 0;
  memcpy(buffer, &checkpoint_hdr, sizeof(fragment_t));
  bytes_to_send += 
    cyclone_serialize_last_applied(cyclone_handle, 
				   (char *)buffer + sizeof(fragment_t));
  // tx and await reply;
  cyclone_tx(socket, (const unsigned char *)buffer, 
	     bytes_to_send, "Checkpoint header send");
  cyclone_rx(socket, (unsigned char *)&reply, sizeof(uint64_t),
	     "Checkpoint rcv");

  int fd = open(fname, O_RDONLY);
  while(bytes_to_send = read(fd, 
			     (char *)buffer + sizeof(fragment_t), 
			     bufbytes - sizeof(fragment_t))) {
    ((fragment_t *)buffer)->offset = checkpoint_hdr.offset;
    // tx and await reply;
    cyclone_tx(socket, (const unsigned char *)buffer, 
	       sizeof(fragment_t) + bytes_to_send, "Checkpoint send");
    cyclone_rx(socket, (unsigned char *)&reply, sizeof(uint64_t),
	       "Checkpoint rcv");
    checkpoint_hdr.offset += bytes_to_send;
  }
  close(fd);
  restore_sigsegv_handler();
  // Transmit pages
  save_page_t *cursor = saved_pages;
  while(cursor != NULL) {
    ((fragment_t *)buffer)->offset = cursor->offset;
    memcpy((char *)buffer + sizeof(fragment_t),
	   cursor->saved_version,
	   pagesize);
    // tx and await reply;
    cyclone_tx(socket, (const unsigned char *)buffer, 
	       sizeof(fragment_t) + pagesize, "Checkpoint send");
    cyclone_rx(socket, (unsigned char *)&reply, sizeof(uint64_t),
	       "Checkpoint rcv");
    cursor = cursor->next;
  }
  //tx EOF and throw away reply;
  ((fragment_t *)buffer)->offset = checkpoint_hdr.offset;
  cyclone_tx(socket, (const unsigned char *)buffer, sizeof(fragment_t),
	     "Checkpoint send");
  cyclone_rx(socket, (unsigned char *)&reply, sizeof(uint64_t),
	     "Checkpoint rcv");
  delete_saved_pages();
}

void init_build_image(void *socket,
		      int *termp,
		      int *indexp,
		      int *masterp,
		      void **init_ety_ptr)
{
  uint64_t reply = REPLY_OK;
  int bytes = cyclone_rx(socket,
			 (unsigned char *)buffer,
			 bufbytes,
			 "Checkpoint rcv");
  memcpy(&checkpoint_hdr, buffer, sizeof(fragment_t));
  *termp  = checkpoint_hdr.last_included_term;
  *indexp = checkpoint_hdr.last_included_index;
  *masterp = checkpoint_hdr.master;
  if(bytes > sizeof(fragment_t)) {
    *init_ety_ptr = (void *)((char *)buffer + sizeof(fragment_t));
  }
  else {
    *init_ety_ptr = NULL;
  }
  cyclone_tx(socket, (const unsigned char *)&reply, sizeof(uint64_t), "Checkpoint send");
}

void build_image(void *socket)
{
  fragment_t * fptr;
  int bytes;
  uint64_t reply;
  checkpoint_hdr.offset = 0;
  int fd_out = open(fname, O_WRONLY|O_TRUNC|O_CREAT, S_IRUSR|S_IWUSR|S_IRGRP|S_IWGRP|S_IROTH|S_IWOTH);
  if(fd_out == -1) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to open output file for checkpoint";
    exit(-1);
  }
  while(true) {
    bytes = cyclone_rx(socket, (unsigned char *)buffer, bufbytes, "Checkpoint rcv");
    fptr = (fragment_t *)buffer;
    if(fptr->term != checkpoint_hdr.term) {
      BOOST_LOG_TRIVIAL(fatal) << "Failed to get checkpoint";
      exit(-1);
    }
    else {
      reply = REPLY_OK;
      if(checkpoint_hdr.offset == 0) {
	// prep file
	lseek(fd_out, 0, SEEK_SET);
	ftruncate(fd_out, 0);
      }
      char * buf = (char *)buffer + sizeof(fragment_t);
      int bytes_left = bytes - sizeof(fragment_t);
      while(bytes_left) {
	int bytes_written = write(fd_out, buf, bytes_left);
	if(bytes_written == 0) {
	  BOOST_LOG_TRIVIAL(fatal)
	    << "Unable to write to checkpoint file";
	  exit(-1);
	}
	bytes_left            -= bytes_written;
	buf                   += bytes_written;
	checkpoint_hdr.offset += bytes_written;
      }
    }
    cyclone_tx(socket, (const unsigned char *)&reply, sizeof(uint64_t), "Checkpoint send");
    if(bytes == sizeof(fragment_t)) {
      break;
    }
  }
  close(fd_out);
}

int image_get_term()
{
  return checkpoint_hdr.last_included_term;
}

int image_get_idx()
{
  return checkpoint_hdr.last_included_index;
}

void delete_checkpoint(void *checkpoint)
{
  munmap(checkpoint, checkpoint_size);
  unlink("/tmp/chkpoint");
  free(buffer);
}
