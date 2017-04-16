#all: counter_server counter_driver counter_coordinator\
counter_loader counter_add_node counter_delete_node\
counter_coordinator_driver counter_driver_mt counter_driver_noop_mt
#all: counter_server counter_driver_noop_mt counter_delete_node counter_add_node counter_loader counter_driver_mt
all: echo_server echo_client echo_client_multicore rocksdb_client rocksdb_client_multicore echo_logserver rocksdb_server rocksdb_loader rocksdb_checkpoint



CXXFLAGS = -O3 -DBOOST_LOG_DYN_LINK
#CXXFLAGS = -O1 -g -fno-omit-frame-pointer -DBOOST_LOG_DYN_LINK

LIBS = -lc -lcyclone /usr/lib/libcraft.a -lboost_system -lboost_date_time -lpmemobj -lpmem -lpthread -laio
#DPDK extras
RTE_SDK=/root/dpdk-stable-16.11.1
CXXFLAGS += -DDPDK_STACK
CXXFLAGS += -march=native -DRTE_MACHINE_CPUFLAG_SSE -DRTE_MACHINE_CPUFLAG_SSE2 -DRTE_MACHINE_CPUFLAG_SSE3 -DRTE_MACHINE_CPUFLAG_SSSE3 -DRTE_MACHINE_CPUFLAG_SSE4_1 -DRTE_MACHINE_CPUFLAG_SSE4_2 -DRTE_MACHINE_CPUFLAG_AES -DRTE_MACHINE_CPUFLAG_PCLMULQDQ -DRTE_MACHINE_CPUFLAG_AVX  -I/root/build/include -I${RTE_SDK}/x86_64-native-linuxapp-gcc/include -include ${RTE_SDK}/x86_64-native-linuxapp-gcc/include/rte_config.h
LIBS += -L/root/build/lib -L${RTE_SDK}/x86_64-native-linuxapp-gcc/lib  -L${RTE_SDK}/x86_64-native-linuxapp-gcc/lib -Wl,--whole-archive -Wl,-lrte_distributor -Wl,-lrte_reorder -Wl,-lrte_kni -Wl,-lrte_pipeline -Wl,-lrte_table -Wl,-lrte_port -Wl,-lrte_timer -Wl,-lrte_hash -Wl,-lrte_jobstats -Wl,-lrte_lpm -Wl,-lrte_power -Wl,-lrte_acl -Wl,-lrte_meter -Wl,-lrte_sched -Wl,-lrte_vhost -Wl,-lm -Wl,--start-group -Wl,-lrte_kvargs -Wl,-lrte_mbuf -Wl,-lrte_ip_frag -Wl,-lrte_ethdev -Wl,-lrte_net -Wl,-lrte_cryptodev -Wl,-lrte_mempool -Wl,-lrte_ring -Wl,-lrte_eal -Wl,-lrte_cmdline -Wl,-lrte_cfgfile -Wl,-lrte_pmd_bond -Wl,-lrte_pmd_vmxnet3_uio -Wl,-lrte_pmd_virtio -Wl,-lrte_pmd_cxgbe -Wl,-lrte_pmd_enic -Wl,-lrte_pmd_i40e -Wl,-lrte_pmd_fm10k -Wl,-lrte_pmd_ixgbe -Wl,-lrte_pmd_e1000 -Wl,-lrte_pmd_ena -Wl,-lrte_pmd_ring -Wl,-lrte_pmd_af_packet -Wl,-lrte_pmd_null -Wl,-lrte_pmd_null_crypto -Wl,-lrte_pmd_vhost -Wl,-ldl -Wl,--end-group -Wl,--no-whole-archive

#Rocksdb 
ROCKS_HOME=/root/cyclone/rocksdb
ROCKS_CXXFLAGS=-std=c++11 -I//root/cyclone/rocksdb/include/ -L/root/cyclone/rocksdb -DROCKSDB_PLATFORM_POSIX -DROCKSDB_LIB_IO_POSIX  -DOS_LINUX -fno-builtin-memcmp -DROCKSDB_FALLOCATE_PRESENT -DSNAPPY -DGFLAGS=gflags -DZLIB -DBZIP2 -DZSTD -DNUMA -DROCKSDB_MALLOC_USABLE_SIZE -DROCKSDB_PTHREAD_ADAPTIVE_MUTEX -DROCKSDB_BACKTRACE -march=native  "-DROCKSDB_JEMALLOC"
ROCKS_LIBS += ${ROCKS_HOME}/librocksdb.a ${ROCKS_HOME}/libbz2.a ${ROCKS_HOME}/libz.a ${ROCKS_HOME}/libsnappy.a -lsnappy -lgflags -lz -lbz2 -lzstd 

BOOST_THREAD_LIB=-lboost_thread-mt
#BOOST_THREAD_LIB=-lboost_thread

common.o:common.cpp
	$(CXX) $(CXXFLAGS) -c common.cpp -o $@

hashmap_tx.o:hashmap_tx.c 
	$(CC) -O2 -std=gnu99 hashmap_tx.c -c -o $@

counter_server:counter_server.cpp common.o hashmap_tx.o
	$(CXX) $(CXXFLAGS) counter_server.cpp common.o hashmap_tx.o $(BOOST_THREAD_LIB) $(LIBS) -o $@

echo_server:echo_server.cpp
	$(CXX) $(CXXFLAGS) echo_server.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

echo_logserver:echo_logserver.cpp
	$(CXX) $(CXXFLAGS) echo_logserver.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rocksdb_server:rocksdb_server.cpp
	$(CXX) $(CXXFLAGS) $(ROCKS_CXXFLAGS) rocksdb_server.cpp $(BOOST_THREAD_LIB) $(LIBS) $(ROCKS_LIBS) -o $@

rocksdb_loader:rocksdb_loader.cpp
	$(CXX) $(CXXFLAGS) $(ROCKS_CXXFLAGS) rocksdb_loader.cpp $(BOOST_THREAD_LIB) $(LIBS) $(ROCKS_LIBS) -o $@

rocksdb_checkpoint:rocksdb_checkpoint.cpp
	$(CXX) $(CXXFLAGS) $(ROCKS_CXXFLAGS) rocksdb_checkpoint.cpp $(BOOST_THREAD_LIB) $(LIBS) $(ROCKS_LIBS) -o $@

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

echo_client:echo_client.cpp 
	$(CXX) $(CXXFLAGS) echo_client.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rocksdb_client:rocksdb_client.cpp 
	$(CXX) $(CXXFLAGS) rocksdb_client.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

rocksdb_client_multicore:rocksdb_client_multicore.cpp 
	$(CXX) $(CXXFLAGS) rocksdb_client_multicore.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

echo_client_multicore:echo_client_multicore.cpp 
	$(CXX) $(CXXFLAGS) echo_client_multicore.cpp $(BOOST_THREAD_LIB) $(LIBS) -o $@

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
counter_loader counter_driver_mt common.o counter_driver_noop_mt \
echo_server echo_client echo_client_multicore rocksdb_server rocksdb_client \
echo_logserver rocksdb_loader rocksdb_checkpoint rocksdb_client_multicore

