#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
rm -f /mnt/tmpfs/dispdata*
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
./rbtree_map_client 0 &> output_client &
echo $!
#sleep 3
cd ../test2
./rbtree_map_client 1 &> output_client &
echo $!
#sleep 1
cd ../test3
./rbtree_map_client 2 &> output_client &
echo $!
