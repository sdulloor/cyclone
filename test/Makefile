all: counter_server counter_driver counter_coordinator\
counter_loader counter_add_node counter_delete_node\
counter_coordinator_driver counter_driver_mt counter_driver_noop_mt




CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

LIBS = -lc -lcyclone -lcraft -lzmq -lboost_system -lboost_date_time -lpmemobj -lpmem -lpthread
#DPDK extras
RTE_SDK=/root/dpdk-16.04
CXXFLAGS += -DDPDK_STACK
CXXFLAGS += -march=native -DRTE_MACHINE_CPUFLAG_SSE -DRTE_MACHINE_CPUFLAG_SSE2 -DRTE_MACHINE_CPUFLAG_SSE3 -DRTE_MACHINE_CPUFLAG_SSSE3 -DRTE_MACHINE_CPUFLAG_SSE4_1 -DRTE_MACHINE_CPUFLAG_SSE4_2 -DRTE_MACHINE_CPUFLAG_AES -DRTE_MACHINE_CPUFLAG_PCLMULQDQ -DRTE_MACHINE_CPUFLAG_AVX  -I/root/build/include -I/root/dpdk-16.04/x86_64-native-linuxapp-gcc/include -include /root/dpdk-16.04/x86_64-native-linuxapp-gcc/include/rte_config.h
LIBS += -L/root/build/lib -L/root/dpdk-16.04/x86_64-native-linuxapp-gcc/lib  -L/root/dpdk-16.04/x86_64-native-linuxapp-gcc/lib -Wl,--whole-archive -Wl,-lrte_distributor -Wl,-lrte_reorder -Wl,-lrte_kni -Wl,-lrte_pipeline -Wl,-lrte_table -Wl,-lrte_port -Wl,-lrte_timer -Wl,-lrte_hash -Wl,-lrte_jobstats -Wl,-lrte_lpm -Wl,-lrte_power -Wl,-lrte_acl -Wl,-lrte_meter -Wl,-lrte_sched -Wl,-lrte_vhost -Wl,-lm -Wl,-lrt -Wl,--start-group -Wl,-lrte_kvargs -Wl,-lrte_mbuf -Wl,-lrte_ip_frag -Wl,-lethdev -Wl,-lrte_cryptodev -Wl,-lrte_mempool -Wl,-lrte_ring -Wl,-lrte_eal -Wl,-lrte_cmdline -Wl,-lrte_cfgfile -Wl,-lrte_pmd_bond -Wl,-lrte_pmd_vmxnet3_uio -Wl,-lrte_pmd_virtio -Wl,-lrte_pmd_cxgbe -Wl,-lrte_pmd_enic -Wl,-lrte_pmd_i40e -Wl,-lrte_pmd_fm10k -Wl,-lrte_pmd_ixgbe -Wl,-lrte_pmd_e1000 -Wl,-lrte_pmd_ena -Wl,-lrte_pmd_ring -Wl,-lrte_pmd_af_packet -Wl,-lrte_pmd_null -Wl,-lrte_pmd_null_crypto -Wl,-lrte_pmd_vhost -Wl,-ldl -Wl,--end-group -Wl,--no-whole-archive


BOOST_THREAD_LIB=-lboost_thread-mt
#BOOST_THREAD_LIB=-lboost_thread

common.o:common.cpp
	$(CXX) $(CXXFLAGS) -c common.cpp -o $@

counter_server:counter_server.cpp common.o
	$(CXX) $(CXXFLAGS) counter_server.cpp common.o ../nvml/src/examples/libpmemobj/tree_map/librbtree_map.a $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_coordinator:counter_coordinator.cpp common.o 
	$(CXX) $(CXXFLAGS) counter_coordinator.cpp common.o $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_loader:counter_loader.cpp 
	$(CXX) $(CXXFLAGS) counter_loader.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_driver:counter_driver.cpp 
	$(CXX) $(CXXFLAGS) counter_driver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_driver_mt:counter_driver_mt.cpp 
	$(CXX) $(CXXFLAGS) counter_driver_mt.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_driver_noop_mt:counter_driver_noop_mt.cpp 
	$(CXX) $(CXXFLAGS) counter_driver_noop_mt.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_coordinator_driver:counter_coordinator_driver.cpp 
	$(CXX) $(CXXFLAGS) counter_coordinator_driver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_delete_node:counter_delete_node.cpp 
	$(CXX) $(CXXFLAGS) counter_delete_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

counter_add_node:counter_add_node.cpp 
	$(CXX) $(CXXFLAGS) counter_add_node.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

.PHONY:clean

clean:
	rm -f counter_server counter_driver \
counter_coordinator counter_delete_node counter_add_node \
counter_loader counter_driver_mt common.o

