#!/bin/bash
if [ $# -ne 2 ]
    then echo "Usage $0 config_dir deploy_dir"
    exit
fi
output_dir=$1
deploy_dir=$2
for i in ${output_dir}/* 
do
    if [ -d "$i" ] ; then
	if [ -f "$i/launch_servers" ] ; then
	    node=$(basename $i)
	    ip=`cat ${i}/ip_address`
	    echo "deploying service on node $node"
	    clush -w ${ip} 'rm -rf /dev/shm/*.cyclone*'
	    clush -w ${ip} ${deploy_dir}/${node}/exec_servers.sh
	fi
	if [ -f "$i/launch_inactive_servers" ] ; then
	    node=$(basename $i)
	    ip=`cat ${i}/ip_address`
	    echo "cleaning up on node $node"
	    clush -w ${ip} 'rm -rf /dev/shm/*.cyclone*'
	fi
    fi
done
