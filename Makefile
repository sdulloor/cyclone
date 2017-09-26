all:
	(cd raft.git && make clean && make;yes | cp include/raft.h /usr/include;yes | cp libcraft.so /usr/lib;yes | cp libcraft.a /usr/lib)
	(cd nvml;make clean;make && make install DESTDIR=/usr/lib/ prefix=..;ldconfig -v)
	(cd core;make clean;make && make install;ldconfig -v)
	(cd test;make clean;make)
