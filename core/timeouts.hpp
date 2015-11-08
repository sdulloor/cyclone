#ifndef _TIMEOUTS_
#define _TIMEOUTS_

// Server side timeouts -- usecs
const int PERIODICITY                = 10; 
const int RAFT_ELECTION_TIMEOUT      = 1000000; 
const int RAFT_REQUEST_TIMEOUT       = 2000; // warning: this should be high enough !

// Client side timeouts
static const int timeout_msec  = 10000; // Client - failure detect

#endif
