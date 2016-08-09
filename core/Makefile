all: libcyclone.a

CXXFLAGS = -O3 -fPIC -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -fno-omit-frame-pointer -g -fPIC -DBOOST_LOG_DYN_LINK 

#Choose your network stack

#Option 1: TCP/IP + ZeroMQ
#CXXFLAGS += -DZMQ_STACK

#Option 2: DPDK
CXXFLAGS += -DDPDK_STACK -std=gnu++0x
RTE_SDK=/root/dpdk-16.04
CXXFLAGS += -march=native -DRTE_MACHINE_CPUFLAG_SSE -DRTE_MACHINE_CPUFLAG_SSE2 -DRTE_MACHINE_CPUFLAG_SSE3 -DRTE_MACHINE_CPUFLAG_SSSE3 -DRTE_MACHINE_CPUFLAG_SSE4_1 -DRTE_MACHINE_CPUFLAG_SSE4_2 -DRTE_MACHINE_CPUFLAG_AES -DRTE_MACHINE_CPUFLAG_PCLMULQDQ -DRTE_MACHINE_CPUFLAG_AVX  -I/root/build/include -I/root/dpdk-16.04/x86_64-native-linuxapp-gcc/include -include /root/dpdk-16.04/x86_64-native-linuxapp-gcc/include/rte_config.h


#Depending on boost version/OS distro
BOOST_THREAD_LIB=-lboost_thread-mt
#BOOST_THREAD_LIB=-lboost_thread


libcyclone.a: libcyclone.o dispatcher.o dispatch_client.o
	ar rcs $@ $^

libcyclone.o: cyclone.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) cyclone.cpp -c -o $@

dispatcher.o: dispatcher.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatcher.cpp -c -o $@

checkpoint.o: checkpoint.cpp checkpoint.hpp
	$(CXX) $(CXXFLAGS) checkpoint.cpp -c -o $@

checkpoint_savepage.o: checkpoint_savepage.cpp checkpoint.hpp
	$(CXX) $(CXXFLAGS) checkpoint_savepage.cpp -c -o $@

dispatch_client.o: dispatch_client.cpp  libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatch_client.cpp -c -o $@

.PHONY:clean install

install:libcyclone.a
	cp libcyclone.a /usr/lib
	cp libcyclone.hpp /usr/include

clean:
	rm -f libcyclone.o dispatcher.o dispatch_client.o \
	checkpoint.o checkpoint_savepage.o libcyclone.a /usr/lib/libcyclone.so \
	/usr/lib/libcyclone.a /usr/include/libcyclone.hpp

