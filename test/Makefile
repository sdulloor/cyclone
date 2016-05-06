all: rbtree_map_server rbtree_map_partitioned_driver rbtree_map_coordinator\
rbtree_map_partitioned_loader rbtree_map_add_node rbtree_map_delete_node

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

LIBS = -lc -lcyclone -lboost_system -lboost_data_time -lpmemobj -lpthread

#BOOST_THREAD_LIB=-lboost_thread-mt
BOOST_THREAD_LIB=-lboost_thread

rbtree_map_server:rbtree_map_server.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_server.cpp ../nvml.git/src/examples/libpmemobj/tree_map/rbtree_map.o $(BOOST_THREAD_LIB) $(LIBS) -o $@

rbtree_map_coordinator:rbtree_map_coordinator.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_coordinator.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rbtree_map_partitioned_loader:rbtree_map_partitioned_loader.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_partitioned_loader.cpp  $(BOOST_THREAD_LIB) $(LIBS) -o $@

rbtree_map_partitioned_driver:rbtree_map_partitioned_driver.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_partitioned_driver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rbtree_map_delete_node:rbtree_map_delete_node.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_delete_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rbtree_map_add_node:rbtree_map_add_node.cpp 
	$(CXX) $(CXXFLAGS) rbtree_map_add_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

.PHONY:clean

clean:
	rm -f rbtree_map_server rbtree_map_partitioned_driver\
rbtree_map_coordinator rbtree_map_delete_node rbtree_map_add_node\
rbtree_map_partitioned_loader

