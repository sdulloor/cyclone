#!/bin/bash
GROUP='sal-sdv[1-4]'

clush -w ${GROUP} killall -9 rbtree_map_server
clush -w ${GROUP} killall -9 rbtree_map_driver
clush -w ${GROUP} killall -9 cyclone_test

