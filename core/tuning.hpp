#ifndef _TUNING_
#define _TUNING_

// Server side timeouts -- usecs
const int PERIODICITY                = 10; 
const int RAFT_ELECTION_TIMEOUT      = 10000; 
const int RAFT_REQUEST_TIMEOUT       = 2000; // this has to be high enough !

// Client side timeouts
static const int timeout_msec  = 30; // Client - failure detect

//Dispatcher batching
const int BATCH_SIZE = 5;
const int DISP_BATCHING_INTERVAL     = 50;


#endif
