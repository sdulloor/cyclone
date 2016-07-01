#ifndef _TUNING_
#define _TUNING_

// Server side timeouts -- usecs
static const int PERIODICITY                = 10; 
static const int RAFT_ELECTION_TIMEOUT      = 10000; 
static const int RAFT_REQUEST_TIMEOUT       = 60; // this has to be high enough !

// Client side timeouts
static const int timeout_msec  = 30; // Client - failure detect

//Dispatcher batching
static const int MIN_BATCH_BUFFERS = 5;
static const int MAX_BATCH_SIZE    = 500;
static const int DISP_BATCHING_INTERVAL = 50;

// ZMQ specific tuning
static const int zmq_threads = 3;

#endif
