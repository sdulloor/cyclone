#!/bin/bash
BASE_DIR=/data/devel/cyclone
CLIENT_SLEEP=0
CLIENTS=4
REPLICAS=4
GROUP=sal-sdv[1-4]


clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/config_sal.ini $BASE_DIR/cyclone_test.ini
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_server $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_driver $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/cyclone_test $BASE_DIR
clush -w ${GROUP} 'rm -rf /mnt/tmpfs/*'
for i in `seq 1 1 $REPLICAS` 
do
id=`expr $i - 1`
INTERP="#!/bin/bash"
CHANGE_DIR="cd $BASE_DIR"
SET_LDPATH="export LD_LIBRARY_PATH=/usr/local/lib"
CMD="./rbtree_map_server $id $REPLICAS $CLIENTS &> output_server &"
clush -w sal-sdv${i} "echo '$INTERP' > $BASE_DIR/server_cmd.sh"
clush -w sal-sdv${i} "echo '$CHANGE_DIR' >> $BASE_DIR/server_cmd.sh"
clush -w sal-sdv${i} "echo '$SET_LDPATH' >> $BASE_DIR/server_cmd.sh"
clush -w sal-sdv${i} "echo '$CMD' >> $BASE_DIR/server_cmd.sh"
clush -w sal-sdv${i} chmod u+x $BASE_DIR/server_cmd.sh
clush -w sal-sdv${i} "echo '$INTERP' > $BASE_DIR/client_cmd.sh"
clush -w sal-sdv${i} "echo '$CHANGE_DIR' >> $BASE_DIR/client_cmd.sh"
clush -w sal-sdv${i} "echo '$SET_LDPATH' >> $BASE_DIR/client_cmd.sh"
clush -w sal-sdv${i} chmod u+x $BASE_DIR/client_cmd.sh
CMD="./cyclone_test $id $REPLICAS &> output_cyclone &"
clush -w sal-sdv${i} "echo '$INTERP' > $BASE_DIR/cyclone_cmd.sh"
clush -w sal-sdv${i} "echo '$CHANGE_DIR' >> $BASE_DIR/cyclone_cmd.sh"
clush -w sal-sdv${i} "echo '$SET_LDPATH' >> $BASE_DIR/cyclone_cmd.sh"
clush -w sal-sdv${i} "echo '$CMD' >> $BASE_DIR/cyclone_cmd.sh"
clush -w sal-sdv${i} chmod u+x $BASE_DIR/cyclone_cmd.sh
done

for i in `seq 1 1 $REPLICAS`
do
for j in `seq $i $REPLICAS $CLIENTS`
do
id=`expr $j - 1`
CMD="./rbtree_map_driver $id $REPLICAS $CLIENTS $CLIENT_SLEEP &> output_client_$id &"
clush -w sal-sdv${i} "echo '$CMD' >> $BASE_DIR/client_cmd.sh"
done
done

# Run rbtree test
clush -w $GROUP $BASE_DIR/server_cmd.sh
sleep 3
clush -w $GROUP $BASE_DIR/client_cmd.sh

# Run cyclone test
#clush -w sal-sdv[1-4] $BASE_DIR/cyclone_cmd.sh 
