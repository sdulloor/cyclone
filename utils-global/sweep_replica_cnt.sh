#!/bin/bash

setup() {
    rm -rf test_output
    r=$1
    c=$2
    p=$3
    echo "Setting up for replicas=$r clients=$c"
    cat ../utils-arch-cluster/example.ini  | sed s"/replicas=3/replicas=$r/" | sed s"/clients=180/clients=$c/" > sweep_setup.ini
    export PAYLOAD=$p
    python config_generator.py  ../utils-arch-cluster/cluster-dpdk.ini sweep_setup.ini noop_dpdk_basic.py test_output

}   

run() {
    ./deploy_configs.sh test_output /root
    ./deploy_shutdown.sh test_output /root
    ./deploy_services.sh test_output /root
    sleep 60
    ./deploy_clients.sh test_output /root
    sleep 120
}

collect() {
    r=$1
    c=$2
    p=$3
    rm -f client_log_${r}_${c}_${p}
    rm -f server_log_${r}_${c}_${p}
    mc0=$((2*$r+2))
    scp arch-h${mc0}:/root/cyclone_$r/client_log0 .
    cp /root/cyclone_0/server_log .
    mv client_log0 client_log_${r}_${c}_${p}
    mv server_log server_log_${r}_${c}_${p}
}

do_test() {
    r=$1
    c=$2
    p=$3
    setup $r $c $p
    run
    collect $r $c $p
    ./deploy_shutdown.sh test_output /root
}

for replicas in 1 2 3
do
    for clients in 1 20 50 100 150 180 200 250 300 350 400
    do
	do_test $replicas $clients 0
	do_test $replicas $clients 512
    done
done

