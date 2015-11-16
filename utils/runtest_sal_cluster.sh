#!/bin/bash
GROUP="sal-sdv[1-4]"
BASE_DIR=/data/devel/cyclone
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/config_sal.ini $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_server $BASE_DIR
clush -w ${GROUP} cp $BASE_DIR/cyclone.git/test/rbtree_map_driver $BASE_DIR
clush -w sal-sdv1 "$BASE_DIR/cyclone.git/test/rbtree_map_server 0 &> output_server &"
clush -w sal-sdv2 "$BASE_DIR/cyclone.git/test/rbtree_map_server 1 &> output_server &"
clush -w sal-sdv3 "$BASE_DIR/cyclone.git/test/rbtree_map_server 2 &> output_server &"
clush -w sal-sdv4 "$BASE_DIR/cyclone.git/test/rbtree_map_server 3 &> output_server &"
sleep 3
clush -w sal-sdv1 "$BASE_DIR/cyclone.git/test/rbtree_map_client 0 &> output_server &"
clush -w sal-sdv2 "$BASE_DIR/cyclone.git/test/rbtree_map_client 1 &> output_server &"
clush -w sal-sdv3 "$BASE_DIR/cyclone.git/test/rbtree_map_client 2 &> output_server &"
clush -w sal-sdv4 "$BASE_DIR/cyclone.git/test/rbtree_map_client 3 &> output_server &"

