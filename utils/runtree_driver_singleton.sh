#!/bin/bash
rm -f /mnt/tmpfs/raftdata*
rm -f /mnt/tmpfs/dispdata*
echo "starting servers"
cd test1
./rbtree_map_server &> output &
sleep 3
echo "starting clients"
cd ../test1
./rbtree_map_driver 0 &> output_client &
echo $!
