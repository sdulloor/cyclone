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
	node=$(basename $i)
	ip=`cat ${i}/ip_address`
	echo "Executing client on node $node"
	clush -w ${ip} ${deploy_dir}/${node}/exec_clients.sh
    fi
done




