#ifndef _TIMEOUTS_
#define _TIMEOUTS_

const int PERIODICITY                = 1; //Raft periodic work
const int RAFT_ELECTION_TIMEOUT      = 1000; 
const int RAFT_REQUEST_TIMEOUT       = 500;

static const int timeout       = 10000; // Client - failure detect
static const int throttle_usec = 10000; // Client - throttle on send socket

#endif
