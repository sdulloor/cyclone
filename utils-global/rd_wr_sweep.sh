#!/bin/bash
ROOT=$PWD
echo "running in $ROOT"
export RBT_KEYS=1000
export RBT_FRAC_READ=0.5  
MAX_CLIENTS=200
MAX_RUN_CLIENTS=1
now=$(date +"%T-%m_%d_%Y")
configs_dir=test_output
deploy_dir=/root
logs_dir="logs"

rm -rf ${now}
rm -rf ${ROOT}/progress

mkdir ${now}
cd ${now}

copy_logs () {
    target=$1
    for i in ${configs_dir}/* 
    do
	if [ -d "$i" ] ; then
	    node=$(basename $i)
	    ip=`cat ${i}/ip_address`
	    if [ -s "$i/launch_clients" ] ; then
		mkdir ${target}/${node}
		scp ${ip}:${deploy_dir}/${node}/client_log* ${target}/${node}
	    fi
	    if [ -s "$i/launch_servers" ] ; then
		mkdir -p ${target}/${node}
		scp ${ip}:${deploy_dir}/${node}/server_log ${target}/${node}
	    fi
	fi
    done
}

shutdown () {
    ${ROOT}/../cyclone.git/utils-global/deploy_shutdown.sh ${configs_dir} ${deploy_dir}
}

mkconfig () {
    cp ${ROOT}/cluster.ini .
    cp ${ROOT}/counter_launcher.py .
    cp ${ROOT}/example.ini .
    python ${ROOT}/../cyclone.git/utils-global/config_generator.py cluster.ini example.ini counter_launcher.py ${configs_dir}
    ${ROOT}/../cyclone.git/utils-global/deploy_shutdown.sh ${configs_dir} ${deploy_dir}
    ${ROOT}/../cyclone.git/utils-global/deploy_configs.sh ${configs_dir} ${deploy_dir}
}

preload () {    
    echo "Preloading $q" >> ${ROOT}/progress
    ${ROOT}/../cyclone.git/utils-global/deploy_services.sh ${configs_dir} ${deploy_dir}
    ${ROOT}/../cyclone.git/utils-global/deploy_preload.sh ${configs_dir} ${deploy_dir}
    echo "Completed preload" >> ${ROOT}/progress
}

runtest () {
    ${ROOT}/../cyclone.git/utils-global/deploy_clients.sh ${configs_dir} ${deploy_dir}
    echo "Running test .. sleeping 2 mins." >> ${ROOT}/progress
    sleep 120
    shutdown
    echo "Completed test" >> ${ROOT}/progress
    mkdir logs
    copy_logs "${logs_dir}"
}



for q in `seq 1 1 1`
do
    quorums_plus_one=$(($q + 1)) # co-ord takes last quorum
    mkdir q_$q
    cd q_$q
    for c in `seq 1 1 $MAX_RUN_CLIENTS` 
    do
	mkdir c_$c
	cd c_$c
	echo "q=$q c=$c" >> ${ROOT}/progress
	echo '[meta]' > ${ROOT}/example.ini
	echo "quorums=$quorums_plus_one" >> ${ROOT}/example.ini
	clients_plus_one=$(($c + 1)) # co-ord takes last client
	echo "clients=$clients_plus_one" >> ${ROOT}/example.ini
	cat ${ROOT}/example_base.ini >> ${ROOT}/example.ini
	mkconfig
	preload
	runtest
	cd ..
    done
    cd ..
done
cd ${ROOT}