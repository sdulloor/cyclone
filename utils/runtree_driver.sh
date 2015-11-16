#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
rm -f /mnt/tmpfs/dispdata*
export LD_LIBRARY_PATH=/usr/local/lib
echo "starting servers"
cd test1
./rbtree_map_server 0 &> output &
echo $!
#sleep 3
cd ../test2
./rbtree_map_server 1 &> output &
echo $!
#sleep 1
cd ../test3
./rbtree_map_server 2 &> output &
echo $!
sleep 3
echo "starting clients"
cd ../test1
./rbtree_map_driver 0 &> output_client &
echo $!
#sleep 3
cd ../test2
./rbtree_map_driver 1 &> output_client &
echo $!
#sleep 1
cd ../test3
./rbtree_map_driver 2 &> output_client &
echo $!
