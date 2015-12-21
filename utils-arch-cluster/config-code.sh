#!/bin/bash

CYCLONE_ROOT_DIR=/root/cyclone
export HTTPS_PROXY="proxy-chain.intel.com:911"
export HTTP_PROXY="proxy-chain.intel.com:911"
export https_proxy="proxy-chain.intel.com:911"
export http_proxy="proxy-chain.intel.com:911"

## Build cyclone
cd $CYCLONE_ROOT_DIR
rm -rf cyclone.git
tar -zxvf cyclone.git.tgz
cd cyclone.git
cd nvml.git
make clean
make && make install prefix=/usr
cd ../core
make clean
make && make install
cd ../test
make clean
make
