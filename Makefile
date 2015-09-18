all: libcyclone.so

CXXFLAGS = -O3 -fPIC

libcyclone.so:libcyclone.o
	$(CXX) -shared -Wl,-soname,libcyclone.so -o libcyclone.so libcyclone.o \
	-lc -lpmemobj -lpmemlog -lzmq


libcyclone.o: cyclone.cpp libcyclone.hpp
	$(CXX) $(CXXFLAGS) cyclone.cpp -c -o libcyclone.o

.PHONY:clean

clean:
	rm -f libcyclone.o libcyclone.so
