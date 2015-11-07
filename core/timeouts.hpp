#ifndef _TIMEOUTS_
#define _TIMEOUTS_

// Server side timeouts -- usecs
const int PERIODICITY                = 10; 
const int RAFT_ELECTION_TIMEOUT      = 1000000; 
const int RAFT_REQUEST_TIMEOUT       = 30;

// Client side timeouts
static const int timeout_msec  = 10000; // Client - failure detect
static const int throttle_usec = 500;  // Client - throttle on send socket

#endif
