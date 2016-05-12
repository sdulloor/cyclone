#!/bin/bash

CYCLONE_ROOT_DIR=/root/cyclone
export HTTPS_PROXY="proxy-chain.intel.com:911"
export HTTP_PROXY="proxy-chain.intel.com:911"
export https_proxy="proxy-chain.intel.com:911"
export http_proxy="proxy-chain.intel.com:911"

# Install packages
yum -y update
yum -y install autoconf
yum -y install libuuid-devel
yum -y groupinstall 'Development Tools'
yum -y install vim
yum -y install emacs
yum -y install python-dev
yum -y install cscope
yum -y install git
yum -y install ctags
yum -y remove 'boost*'
yum -y install 'boost*'
cat /usr/include/boost/exception_ptr.hpp | sed 's/exception_ptr()/~exception_ptr() throw() {} exception_ptr()/' > /tmp/patched
\cp -f /tmp/patched /usr/include/boost/exception_ptr.hpp
yum -y install ntp

service ntpd restart
chkconfig ntpd on

# Build zmq
rm -rf $CYCLONE_ROOT_DIR/zeromq
mkdir $CYCLONE_ROOT_DIR/zeromq
cd $CYCLONE_ROOT_DIR/zeromq
wget --no-check-certificate https://download.libsodium.org/libsodium/releases/libsodium-1.0.3.tar.gz 
tar -zxvf libsodium-1.0.3.tar.gz 
cd libsodium-1.0.3
./configure --libdir=/usr/lib
make
make install
cd ..
wget --no-check-certificate http://download.zeromq.org/zeromq-4.1.3.tar.gz
tar -zxvf zeromq-4.1.3.tar.gz
cd zeromq-4.1.3
./configure --libdir=/usr/lib --without-libsodium
make
make install

