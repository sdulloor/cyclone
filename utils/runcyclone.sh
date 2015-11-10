#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
cd test1
./cyclone_test 0 &>  output &
echo $!
#sleep 3
cd ../test2
./cyclone_test 1 &>   output &
echo $!
#sleep 1
cd ../test3
./cyclone_test 2 &> output &
echo $!
