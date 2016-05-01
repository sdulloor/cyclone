#ifndef _TIMEOUTS_
#define _TIMEOUTS_

// Server side timeouts -- usecs
const int PERIODICITY                = 10; 
const int RAFT_ELECTION_TIMEOUT      = 10000; 
const int RAFT_REQUEST_TIMEOUT       = 2000; // warning: this should be high enough !
const int DISP_BATCHING_INTERVAL     = 50;

// Client side timeouts
static const int timeout_msec  = 30; // Client - failure detect

#endif
