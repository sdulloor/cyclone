#!/bin/bash

CYCLONE_ROOT_DIR=/data/devel/cyclone/

# Install packages
export DEBIAN_FRONTEND=noninteractive 
apt-get -y update
apt-get -y install autoconf
apt-get -y install uuid-dev
apt-get -y install build-essential
apt-get -y install vim
apt-get -y install emacs
apt-get -y install python-dev
apt-get -y install cscope
apt-get -y install git
apt-get -y install ctags
apt-get -y install libboost.*1.55.*dev
apt-get -y install ntp

service ntp reload

# Build zmq
rm -rf $CYCLONE_ROOT_DIR/zeromq
mkdir $CYCLONE_ROOT_DIR/zeromq
cd $CYCLONE_ROOT_DIR/zeromq
wget https://download.libsodium.org/libsodium/releases/libsodium-1.0.3.tar.gz 
tar -zxvf libsodium-1.0.3.tar.gz 
cd libsodium-1.0.3
./configure --libdir=/usr/lib
make
make install
cd ..
wget http://download.zeromq.org/zeromq-4.1.3.tar.gz
tar -zxvf zeromq-4.1.3.tar.gz
cd zeromq-4.1.3
./configure --libdir=/usr/lib
make
make install

# Build nvml
cd $CYCLONE_ROOT_DIR
rm -rf nvml.git
git clone https://github.com/pmem/nvml/ nvml.git 
cd nvml.git
make && make install prefix=/usr

# Build raft
cd $CYCLONE_ROOT_DIR
git clone https://github.com/willemt/raft raft.git 
cd raft.git
make
cp include/raft.h /usr/include/ 
cp libcraft.* /usr/lib/ 

## Build cyclone
#cd $CYCLONE_ROOT_DIR
#git clone https://github.com/sdulloor/cyclone cyclone.git
#cd cyclone.git/core
#make && make install
