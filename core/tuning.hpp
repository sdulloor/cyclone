#ifndef _TUNING_
#define _TUNING_

// Server side timeouts -- usecs
static const int PERIODICITY                = 10; 
static const int RAFT_ELECTION_TIMEOUT      = 10000; 
static const int RAFT_REQUEST_TIMEOUT       = 1000; 
static const int RAFT_NACK_TIMEOUT          = 60;
// RAFT log tuning -- need to match load
static const int RAFT_LOG_TARGET  = 5000;

// Client side timeouts
static const int timeout_msec  = 30; // Client - failure detect

//Dispatcher batching
static const int MIN_BATCH_BUFFERS = 2;
static const int MAX_BATCH_SIZE    = 5; // Ultimately bounded by MAX_MSGSIZE
static const int DISP_BATCHING_INTERVAL = 10;
static const int executor_threads = 8;

// ZMQ specific tuning
static const int zmq_threads = 4;

// DPDK specific tuning
static const int q_junk       = 0;
static const int q_raft       = 1;
static const int q_dispatcher = 2;
static const int q_control    = 3;
static const int q_client     = 4;
static const int num_queues   = 5;


#endif
