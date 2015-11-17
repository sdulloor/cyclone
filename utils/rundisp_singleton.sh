#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
rm -f /mnt/tmpfs/dispdata*
echo "starting servers"
cd test1
./dispatch_test_server 0 1 1 &> output &
echo $!
sleep 3
echo "starting clients"
cd ../test1
./dispatch_test_client 0 1 1 &> output_client &
echo $!
