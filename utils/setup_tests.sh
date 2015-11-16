#!/bin/bash

mkdir test1
mkdir test2
mkdir test3

cp test/cyclone_test test1/
cp test/cyclone_test test2/
cp test/cyclone_test test3/

cp test/dispatch_test_server test1/
cp test/dispatch_test_client test1/
cp test/dispatch_test_server test2/
cp test/dispatch_test_client test2/
cp test/dispatch_test_server test3/
cp test/dispatch_test_client test3/

cp test/rbtree_map_server test1/
cp test/rbtree_map_client test1/
cp test/rbtree_map_driver test1/
cp test/rbtree_map_server test2/
cp test/rbtree_map_client test2/
cp test/rbtree_map_driver test2/
cp test/rbtree_map_server test3/
cp test/rbtree_map_client test3/
cp test/rbtree_map_driver test3/

cp test/config_local.ini test1/cyclone_test.ini
cp test/config_local.ini test2/cyclone_test.ini
cp test/config_local.ini test3/cyclone_test.ini
