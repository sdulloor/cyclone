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
make
