#!/bin/bash
if [ $# -ne 2 ]
    then echo "Usage $0 deploy_dir group"
    exit
fi
deploy_dir=$1
GROUP=$2
echo "deploy dir = $deploy_dir"
echo "group = $GROUP"
echo '#!/bin/bash' > exec_client.sh
echo "export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib" >> exec_client.sh
echo "cd $deploy_dir" >> exec_client.sh
cp exec_client.sh exec_preload.sh
cp exec_client.sh exec_tx_client.sh
chmod u+x exec_client.sh
chmod u+x exec_tx_client.sh
chmod u+x exec_preload.sh
echo 'source launch_preload' >> exec_preload.sh
echo 'source launch_client'  >> exec_client.sh
echo 'source launch_tx_client' >> exec_tx_client.sh
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/rbtree_map_coordinator_load ${deploy_dir}
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/rbtree_map_coordinator_driver ${deploy_dir}
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/rbtree_map_partitioned_driver ${deploy_dir}
clush -w ${GROUP} --copy exec_tx_client.sh --dest ${deploy_dir}
clush -w ${GROUP} --copy exec_client.sh --dest ${deploy_dir}
clush -w ${GROUP} --copy exec_preload.sh --dest ${deploy_dir}


