all: cyclone_test dispatch_test_server dispatch_test_client

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -g -DBOOST_LOG_DYN_LINK

cyclone_test:cyclone_test.cpp 
	$(CXX) $(CXXFLAGS) cyclone_test.cpp -lc -lcyclone -lboost_thread -lboost_log -lpthread -o $@

dispatch_test_server:dispatch_test_server.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_server.cpp -lc -lcyclone -lboost_thread -lboost_log -lpthread -o $@

dispatch_test_client:dispatch_test_client.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_client.cpp -lzmq -lc -lcyclone -lboost_thread -lboost_log -lpthread -o $@

.PHONY:clean

clean:
	rm -f cyclone_test dispatch_test_server dispatch_test_client
