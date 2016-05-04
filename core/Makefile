all: libcyclone.so

CXXFLAGS = -O3 -fPIC -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -fno-omit-frame-pointer -g -fPIC -DBOOST_LOG_DYN_LINK

#BOOST_THREAD_LIB=-lboost_thread-mt
BOOST_THREAD_LIB=-lboost_thread

libcyclone.so: libcyclone.o dispatcher.o dispatcher_exec.o dispatch_client.o \
	checkpoint.o checkpoint_savepage.o
	$(CXX) -shared -Wl,-soname,libcyclone.so -o libcyclone.so libcyclone.o\
	 dispatcher.o dispatcher_exec.o dispatch_client.o checkpoint.o \
	checkpoint_savepage.o -lc -lcraft -lpmemobj -lpmemlog -lpmem -lzmq \
	-lboost_system -lboost_date_time $(BOOST_THREAD_LIB)

libcyclone.o: cyclone.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) cyclone.cpp -c -o $@

dispatcher.o: dispatcher.cpp dispatcher_exec.hpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) dispatcher.cpp -c -o $@

checkpoint.o: checkpoint.cpp checkpoint.hpp
	$(CXX) $(CXXFLAGS) checkpoint.cpp -c -o $@

checkpoint_savepage.o: checkpoint_savepage.cpp checkpoint.hpp
	$(CXX) $(CXXFLAGS) checkpoint_savepage.cpp -c -o $@

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
	checkpoint.o checkpoint_savepage.o libcyclone.so

