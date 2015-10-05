all: libcyclone.so

CXXFLAGS = -O3 -fPIC -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -g -fPIC -DBOOST_LOG_DYN_LINK

#CXXFLAGS += -DTRACING

libcyclone.so:libcyclone.o
	$(CXX) -shared -Wl,-soname,libcyclone.so -o libcyclone.so libcyclone.o \
	-lc -lcraft -lpmemobj -lpmemlog -lzmq -lboost_thread -lboost_log


libcyclone.o: cyclone.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) cyclone.cpp -c -o libcyclone.o

.PHONY:clean install

install:libcyclone.so
	cp libcyclone.so /usr/lib
	cp libcyclone.hpp /usr/include

clean:
	rm -f libcyclone.o libcyclone.so
