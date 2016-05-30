all: counter_server counter_driver counter_coordinator\
counter_loader counter_add_node counter_delete_node\
counter_coordinator_driver

CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

LIBS = -lc -lcyclone -lboost_system -lboost_date_time -lpmemobj -lpmem -lpthread

BOOST_THREAD_LIB=-lboost_thread-mt
#BOOST_THREAD_LIB=-lboost_thread

common.o:common.cpp
	$(CXX) $(CXXFLAGS) -c common.cpp -o $@

counter_server:counter_server.cpp common.o
	$(CXX) $(CXXFLAGS) counter_server.cpp common.o ../nvml/src/examples/libpmemobj/tree_map/rbtree_map.o $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_coordinator:counter_coordinator.cpp common.o 
	$(CXX) $(CXXFLAGS) counter_coordinator.cpp common.o $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_loader:counter_loader.cpp 
	$(CXX) $(CXXFLAGS) counter_loader.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_driver:counter_driver.cpp 
	$(CXX) $(CXXFLAGS) counter_driver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_coordinator_driver:counter_coordinator_driver.cpp 
	$(CXX) $(CXXFLAGS) counter_coordinator_driver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_delete_node:counter_delete_node.cpp 
	$(CXX) $(CXXFLAGS) counter_delete_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_add_node:counter_add_node.cpp 
	$(CXX) $(CXXFLAGS) counter_add_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

.PHONY:clean

clean:
	rm -f counter_server counter_driver\
counter_coordinator counter_delete_node counter_add_node\
counter_loader common.o

