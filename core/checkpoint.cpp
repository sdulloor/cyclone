#include<sys/mman.h>
#include<sys/types.h>
#include<sys/stat.h>
#include<sys/fcntl.h>
#include "checkpoint.hpp"
static const char fname[500];
static struct fragment_t checkpoint_hdr;
static void *checkpoint;
const int bufbytes = 4*1024*1024;
static void *buffer;
static unsigned long checkpoint_size;

void init_checkpoint(const char *fname_in)
{
  strncpy(fname, fname_in, 50);
  buffer = malloc(bufbytes);
}

int take_checkpoint(void ** checkpoint,
		    int leader_term,
		    int raft_idx,
		    int raft_term)
{
  checkpoint_hdr.term = leader_term;
  checkpoint_hdr.last_included_index = raft_idx;
  checkpoint_hdr.last_included_term = raft_term;
  checkpoint_size = 0;
  // Dumb synchronous file copy for now.
  int fd_in = open(fname, O_RDONLY);
  if(fd_in == -1) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to open input file for checkpoint";
    exit(-1);
  }
  int fd_out = open("/tmp/chkpoint", O_WRONLY|O_TRUNC|O_CREAT, S_IRWXU);
  if(fd_out == -1) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to open output file for checkpoint";
    exit(-1);
  }
  int bytes_read, bytes_written;
  while(bytes_read = read(fd_in, buffer, bufbytes)) {
    checkpoint_size += bytes_read;
    while(bytes_read) {
      bytes_written = write(fd_out, buffer, bytes_read);
      if(bytes_written == 0) {
	BOOST_LOG_TRIVIAL(fatal) << "Unable to write to checkpoint file";
	exit(-1);
      }
      bytes_read -= bytes_written;
    }
  }
  close(fd_in);
  checkpoint = mmap(NULL, checkpoint_size, PROT_READ|PROT_WRITE, MAP_SHARED,
		    fd_in, 0);
  if(checkpoint == MAP_FAILED) {
    BOOST_LOG_TRIVIAL(fatal) << "Unable to mmap checkpoint file";
    exit(-1);
  }
  close(fd_out);
}

void send_checkpoint(void *socket)
{
  uint64_t reply;
  int bytes_to_send;
  memcpy(buffer, &checkpoint_hdr, sizeof(fragment_t));
  checkpoint_hdr.offset = 0;
  
  while(checkpoint_hdr.offset != checkpoint_size) {
    bytes_to_send = checkpoint_size - checkpoint_hdr.offset;
    if((bytes_to_send + sizeof(fragment_t)) > bufbytes) {
      bytes_to_send = bufbytes - sizeof(fragment_t);
    }
    ((fragment_t *)buffer)->offset = checkpoint_hdr.offset;
    memcpy(buffer + sizeof(fragment_t),
	   checkpoint + checkpoint_hdr.offset,
	   bytes_to_send);
    // tx and await reply;
    cyclone_tx(socket, buffer, sizeof(fragment_t) + bytes_to_send);
    cyclone_rx(socket, &reply, sizeof(uint64_t));
    if(reply == REPLY_STALE) { // no longer leader
      break;
    }
    checkpoint_hdr.offset += bytes_to_send;
  }
  // tx EOF and throw away reply;
  ((fragment_t *)buffer)->offset = checkpoint_hdr.offset;
  cyclone_tx(socket, buffer, sizeof(fragment_t));
  cyclone_rx(socket, &reply, sizeof(uint64_t));
}

void build_image(void *socket)
{
  fragment_t * fptr;
  int bytes;
  uint64_t reply;
  checkpoint_hdr.term   = 0;
  checkpoint_hdr.offset = 0;
  int fd_out = open(fname, O_WRONLY|O_TRUNC|O_CREAT, S_IRWXU);
  if(fd_out == -1) {
    BOOST_LOG_TRIVIAL(fatal) << "Failed to open output file for checkpoint";
    exit(-1);
  }
  while(true) {
    bytes = cyclone_rx(socket, buffer, bufbytes);
    fptr = (fragment_t *)buffer;
    if(fptr->term < checkpoint_hdr.term) {
      reply = REPLY_STALE;
    }
    else {
      reply = REPLY_OK;
       if(fptr->term > checkpoint_hdr.term) {
	 // prep file
	 fseek(fd_out, 0, SEEK_SET);
	 ftruncate(fd_out, 0);
	 chkpoint_hdr.offset = 0;
       }
       void * buf = buffer + sizeof(fragment_t);
       int bytes_left = bytes - sizeof(fragment_t);
       while(bytes_left) {
	 int bytes_written = write(fd_out, buffer, bytes_left);
	 if(bytes_written == 0) {
	   BOOST_LOG_TRIVIAL(fatal)
	     << "Unable to write to checkpoint file";
	   exit(-1);
	 }
	 bytes_left            -= bytes_written;
	 checkpoint_hdr.offset += bytes_written;
       }
    }
    cyclone_tx(socket, &reply, sizeof(uint64_t));
    if(bytes == sizeof(fragment_t)) {
      break;
    }
  }
  close(fd_out);
}

void delete_checkpoint(void *checkpoint)
{
  munmap(checkpoint, checkpoint_size);
  unlink("/tmp/chkpoint");
}
