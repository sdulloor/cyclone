#!/bin/bash
BASE_DIR=/root/cyclone
CLIENT_SLEEP=0
CLIENTS=3
REPLICAS=3
PARTITIONS=3
COORDS=3
GROUP='arch-h[2-6/2].dp.intel.com'

clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_archc.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/utils-arch-cluster/config_archc_client.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_coordinator $BASE_DIR
clush -w ${GROUP} 'rm -rf /dev/shm/disp*_c'
clush -w ${GROUP} 'rm -rf /dev/shm/raft*_c'

mc=2
for i in `seq 1 1 $COORDS`
do
    id=$(($i - 1))
    INTERP="#!/bin/bash"
    CHANGE_DIR="cd $BASE_DIR"
    SET_LDPATH="export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib"
    CMD="./rbtree_map_coordinator $id $COORDS $CLIENTS $PARTITIONS $REPLICAS config_archc.ini config_archc_client.ini config_arch config_arch_client &> output_coord &"
    clush -w arch-h${mc} "echo '$INTERP' > $BASE_DIR/coord_cmd.sh"
    clush -w arch-h${mc} "echo '$CHANGE_DIR' >> $BASE_DIR/coord_cmd.sh"
    clush -w arch-h${mc} "echo '$SET_LDPATH' >> $BASE_DIR/coord_cmd.sh"
    clush -w arch-h${mc} "echo '$CMD' >> $BASE_DIR/coord_cmd.sh"
    clush -w arch-h${mc} chmod u+x $BASE_DIR/coord_cmd.sh
    mc=$(($mc + 2))
done



# Run servers
clush -w $GROUP $BASE_DIR/coord_cmd.sh
sleep 3

