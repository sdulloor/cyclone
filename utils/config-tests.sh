#!/bin/bash

CYCLONE_ROOT_DIR=/data/devel/cyclone/

cd ${CYCLONE_ROOT_DIR}/core
make clean
make install

cd ${CYCLONE_ROOT_DIR}/test
make clean
make

cd ${CYCLONE_ROOT_DIR}
mkdir -p test-dir
cp test/rbtree_map_server test-dir
cp test/rbtree_map_client test-dir
