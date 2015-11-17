#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
rm -f /mnt/tmpfs/dispdata*
echo "starting servers"
cd test1
./dispatch_test_server 0 3 3 &> output &
echo $!
#sleep 3
cd ../test2
./dispatch_test_server 1 3 3 &> output &
echo $!
#sleep 1
cd ../test3
./dispatch_test_server 2 3 3 &> output &
echo $!
sleep 3
echo "starting clients"
cd ../test1
./dispatch_test_client 0 3 3 &> output_client &
echo $!
#sleep 3
cd ../test2
./dispatch_test_client 1 3 3 &> output_client &
echo $!
#sleep 1
cd ../test3
./dispatch_test_client 2 3 3 &> output_client &
echo $!
