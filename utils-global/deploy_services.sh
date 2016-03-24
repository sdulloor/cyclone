#!/bin/bash
if [ $# -ne 3 ]
    then echo "Usage $0 output_dir deploy_dir group"
    exit
fi
output_dir=$1
deploy_dir=$2
GROUP=$3
echo "output dir = $output_dir"
echo "deploy dir = $deploy_dir"
echo "group = $GROUP"
echo '#!/bin/bash' > exec_servers.sh
echo "export LD_LIBRARY_PATH=/usr/local/lib:/usr/lib" >> exec_servers.sh
echo "cd $deploy_dir" >> exec_servers.sh
cp exec_servers.sh exec_coord.sh
echo "source launch_servers" >> exec_servers.sh
echo "source launch_coord" >> exec_coord.sh
chmod u+x exec_servers.sh
chmod u+x exec_coord.sh
clush -w ${GROUP} echo " " > ${deploy_dir}/launch_coord
for i in ${output_dir}/* 
do
    if [ -d "$i" ] ; then
	mc=$(basename $i)
	echo "deploying to $mc"
	scp ${i}/* ${mc}:${deploy_dir}
	scp exec_servers.sh ${mc}:
	scp exec_coord.sh ${mc}:
	scp ${output_dir}/*.ini ${mc}:${deploy_dir}
	
    fi
done

clush -w ${GROUP} 'rm -rf /dev/shm/*.cyclone*'
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/rbtree_map_server ${deploy_dir}
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/rbtree_map_coordinator ${deploy_dir}
clush -w ${GROUP} ./exec_servers.sh
echo "Deployed servers sleeping 10 sec ..."
sleep 10
clush -w ${GROUP} ./exec_coord.sh
