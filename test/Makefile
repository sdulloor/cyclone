Ball: cyclone_test

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -g 

cyclone_test:cyclone_test.cpp
	$(CXX) $(CXXFLAGS) cyclone_test.cpp -lc -lcyclone -lboost_thread -lboost_log -lpthread -o $@

.PHONY:clean

clean:
	rm -f cyclone_test
