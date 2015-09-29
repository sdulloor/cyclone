all: cyclone_test

CXXFLAGS = -O3 
#CXXFLAGS = -g 

cyclone_test:cyclone_test.cpp
	$(CXX) $(CXXFLAGS) cyclone_test.cpp -lcyclone -o $@

.PHONY:clean

clean:
	rm -f cyclone_test
