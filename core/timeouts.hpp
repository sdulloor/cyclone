#ifndef _TIMEOUTS_
#define _TIMEOUTS_

const int PERIODICITY                = 10; 
const int RAFT_ELECTION_TIMEOUT      = 1000000; 
const int RAFT_REQUEST_TIMEOUT       = 30;

static const int timeout       = 10000; // Client - failure detect
static const int throttle_usec = 10000;  // Client - throttle on send socket

#endif
