#ifndef _TUNING_
#define _TUNING_

// Server side timeouts -- usecs
static const int PERIODICITY                = 10; 
static const int RAFT_ELECTION_TIMEOUT      = 10000; 
static const int RAFT_REQUEST_TIMEOUT       = 1000; 
static const int RAFT_NACK_TIMEOUT          = 60;

// Client side timeouts
static const int timeout_msec  = 30; // Client - failure detect

//Dispatcher batching
static const int MIN_BATCH_BUFFERS = 2;
static const int MAX_BATCH_SIZE    = 5; // Ultimately bounded by MAX_MSGSIZE
static const int DISP_BATCHING_INTERVAL = 10;

// ZMQ specific tuning
static const int zmq_threads = 3;

#endif
