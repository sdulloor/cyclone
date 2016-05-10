#!/bin/bash
if [ $# -ne 3 ]
    then echo "Usage $0 output_dir deploy_dir group"
    exit
fi
output_dir=$1
deploy_dir=$2
GROUP=$3
clush -w ${GROUP} 'rm -rf /dev/shm/*.cyclone*'
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/counter_server ${deploy_dir}
clush -w ${GROUP} cp ${deploy_dir}/cyclone.git/test/counter_coordinator ${deploy_dir}
clush -w ${GROUP} ./exec_servers.sh
echo "Deployed servers sleeping 5 sec ..."
sleep 5
clush -w ${GROUP} ./exec_coord.sh
