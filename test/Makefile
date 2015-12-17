all: cyclone_test dispatch_test_server dispatch_test_client rbtree_map_server rbtree_map_client rbtree_map_driver

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

cyclone_test:cyclone_test.cpp 
	$(CXX) $(CXXFLAGS) cyclone_test.cpp -lc -lcyclone -lboost_system -lboost_thread -lboost_log -lpthread -o $@

dispatch_test_server:dispatch_test_server.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_server.cpp -lc -lcyclone -lboost_thread -lboost_log -lpthread -o $@

rbtree_map_server:rbtree_map_server.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_server.cpp ../nvml.git/src/examples/libpmemobj/tree_map/rbtree_map.o -lc -lcyclone -lboost_thread -lboost_log -lpthread -lpmemobj -o $@

rbtree_map_client:rbtree_map_client.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_client.cpp -lc -lcyclone -lboost_system -lboost_thread -lboost_log -lpthread -o $@

rbtree_map_driver:rbtree_map_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_driver.cpp -lc -lcyclone -lboost_system -lboost_thread -lboost_log -lpthread -o $@

dispatch_test_client:dispatch_test_client.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_client.cpp -lc -lcyclone -lboost_system -lboost_thread -lboost_log -lpthread -o $@



.PHONY:clean

clean:
	rm -f cyclone_test dispatch_test_server dispatch_test_client rbtree_map_server rbtree_map_client rbtree_map_driver
