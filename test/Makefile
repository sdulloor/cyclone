all: cyclone_test dispatch_test_server dispatch_test_client rbtree_map_server \
rbtree_map_client rbtree_map_driver rbtree_map_synchronous_driver \
rbtree_map_ro_driver rbtree_map_partitioned_driver rbtree_map_coordinator\
rbtree_map_coordinator_driver rbtree_map_coordinator_load rbtree_map_delete_node\
rbtree_map_add_node rbtree_map_partitioned_loader

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

#BOOST_THREAD_LIB=-lboost_thread-mt
BOOST_THREAD_LIB=-lboost_thread

cyclone_test:cyclone_test.cpp 
	$(CXX) $(CXXFLAGS) cyclone_test.cpp -lc -lcyclone $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

dispatch_test_server:dispatch_test_server.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_server.cpp -lc -lcyclone $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_server:rbtree_map_server.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_server.cpp ../nvml.git/src/examples/libpmemobj/tree_map/rbtree_map.o -lc -lcyclone $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpmemobj -o $@

rbtree_map_client:rbtree_map_client.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_client.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_partitioned_loader:rbtree_map_partitioned_loader.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_partitioned_loader.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_driver:rbtree_map_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_driver.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_coordinator:rbtree_map_coordinator.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_coordinator.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_synchronous_driver:rbtree_map_synchronous_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_synchronous_driver.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_ro_driver:rbtree_map_ro_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_ro_driver.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_partitioned_driver:rbtree_map_partitioned_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_partitioned_driver.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_delete_node:rbtree_map_delete_node.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_delete_node.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_add_node:rbtree_map_add_node.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_add_node.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_coordinator_driver:rbtree_map_coordinator_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_coordinator_driver.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

rbtree_map_coordinator_load:rbtree_map_coordinator_load.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_coordinator_load.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@

dispatch_test_client:dispatch_test_client.cpp 
	$(CXX) $(CXXFLAGS) dispatch_test_client.cpp -lc -lcyclone  $(BOOST_THREAD_LIB)  -lboost_system -lboost_date_time -lpthread -o $@



.PHONY:clean

clean:
	rm -f cyclone_test dispatch_test_server dispatch_test_client \
rbtree_map_server rbtree_map_client rbtree_map_driver \
rbtree_map_synchronous_driver rbtree_map_ro_driver \
rbtree_map_partitioned_driver rbtree_map_coordinator \
rbtree_map_coordinator_driver rbtree_map_coordinator_load \
rbtree_map_delete_node rbtree_map_add_node rbtree_map_partitioned_loader


