all: libcyclone.so

CXXFLAGS = -O3 -fPIC -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -fno-omit-frame-pointer -g -fPIC -DBOOST_LOG_DYN_LINK

#CXXFLAGS += -DTRACING

#BOOST_THREAD_LIB=-lboost_thread-mt
BOOST_THREAD_LIB=-lboost_thread

#BOOST_LOG_LIB=
BOOST_LOG_LIB=-lboost_log


libcyclone.so: libcyclone.o dispatcher.o dispatcher_exec.o dispatch_client.o
	$(CXX) -shared -Wl,-soname,libcyclone.so -o libcyclone.so libcyclone.o\
	 dispatcher.o dispatcher_exec.o dispatch_client.o \
	-lc -lcraft -lpmemobj -lpmemlog -lzmq -lboost_system $(BOOST_THREAD_LIB) $(BOOST_LOG_LIB)

libcyclone.o: cyclone.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) cyclone.cpp -c -o $@

dispatcher.o: dispatcher.cpp dispatcher_exec.hpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatcher.cpp -c -o $@

dispatcher_exec.o: dispatcher_exec.cpp dispatcher_exec.hpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatcher_exec.cpp -c -o $@

dispatch_client.o: dispatch_client.cpp  libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatch_client.cpp -c -o $@

.PHONY:clean install

install:libcyclone.so
	cp libcyclone.so /usr/lib
	cp libcyclone.hpp /usr/include

clean:
	rm -f libcyclone.o dispatcher.o dispatcher_exec.o dispatch_client.o \
	libcyclone.so

