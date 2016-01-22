#!/bin/bash
BASE_DIR=/root/cyclone
CLIENT_SLEEP=0
CLIENTS=3
REPLICAS=3
PARTITIONS=3
GROUP='arch-h[2-18/2].dp.intel.com'

clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch0.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch1.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch2.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch_client0.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch_client1.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_arch_client2.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_server $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_partitioned_driver $BASE_DIR
clush -w ${GROUP} 'rm -rf /dev/shm/disp*'
clush -w ${GROUP} 'rm -rf /dev/shm/raft*'

mc=2
suffix=0
for i in `seq 1 1 $PARTITIONS`
do
    for j in `seq 1 1 $REPLICAS` 
    do
	id=$(($j - 1))
	INTERP="#!/bin/bash"
	CHANGE_DIR="cd $BASE_DIR"
	SET_LDPATH="export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib"
	CMD="./rbtree_map_server $id $REPLICAS $CLIENTS config_arch${suffix}.ini config_arch_client${suffix}.ini &> output_server &"
	clush -w arch-h${mc} "echo '$INTERP' > $BASE_DIR/server_cmd.sh"
	clush -w arch-h${mc} "echo '$CHANGE_DIR' >> $BASE_DIR/server_cmd.sh"
	clush -w arch-h${mc} "echo '$SET_LDPATH' >> $BASE_DIR/server_cmd.sh"
	clush -w arch-h${mc} "echo '$CMD' >> $BASE_DIR/server_cmd.sh"
	clush -w arch-h${mc} chmod u+x $BASE_DIR/server_cmd.sh
	mc=$(($mc + 2))
    done
    suffix=$(($suffix + 1))
done



# Run servers
clush -w $GROUP $BASE_DIR/server_cmd.sh
sleep 3

