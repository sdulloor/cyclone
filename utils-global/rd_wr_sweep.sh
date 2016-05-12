#!/bin/bash
ROOT=$PWD
echo "running in $ROOT"
rm -rf sweep_results*
rm -rf progress
KEYS=1000000
MAX_CLIENTS=36
MAX_RUN_CLIENTS=28
for q in `seq 1 1 4`
do
    ${ROOT}/../killtest_arch_cluster.sh
    mkdir sweep_results_q_$q
    cd sweep_results_q_$q
    mkdir preload
    cd preload
    echo '[meta]' > example.ini
    echo "quorums=$q" >> example.ini
    echo "clients=$MAX_CLIENTS" >> example.ini
    cat ${ROOT}/example_base.ini >> example.ini
    cp ${ROOT}/cluster.ini .
    python ${ROOT}/../cyclone.git/utils-global/config_generator.py cluster.ini example.ini test_output
    clush -w arch-h[2-24/2] rm -f exec_servers.sh
    clush -w arch-h[2-24/2] rm -f exec_coord.sh
    clush -w arch-h[2-24/2] rm -f /root/cyclone/server_log
    ${ROOT}/../cyclone.git/utils-global/deploy_configs.sh test_output /root/cyclone arch-h[2-24/2]
    ${ROOT}/../cyclone.git/utils-global/deploy_services.sh test_output /root/cyclone arch-h[2-24/2]
    ${ROOT}/../cyclone.git/utils-global/deploy_driver.sh /root/cyclone arch-h[2-24/2]
    clush -w arch-h[2-24/2] "echo export RBT_KEYS=$KEYS > /tmp/x"
    clush -w arch-h[2-24/2] "echo export RBT_FRAC_READ=$rd_frac_float >> /tmp/x"
    clush -w arch-h[2-24/2] "cat /root/cyclone/launch_preload >> /tmp/x"
    clush -w arch-h[2-24/2] "mv /tmp/x /root/cyclone/launch_preload"
    echo "Preloading $q" >> ${ROOT}/progress
    clush -w arch-h[2-24/2] /root/cyclone/exec_preload.sh
    mc=2
    last_c=$(($MAX_CLIENTS - 1))
    for mx in `seq 0 1 $last_c`
    do
	scp arch-h${mc}:/root/cyclone/client_log${mx} client_log${mx}_${mc}
	mc=$(($mc + 2))
	if [ "$mc" -gt "24" ]; then
	    mc=2
	fi
    done
    echo "Complete preload" >> ${ROOT}/progress
    cd ..
    for rd_frac in `seq 10 20 90`
    do
	rd_frac_float=`echo $rd_frac/100 | bc -l`
	mkdir rd_frac_${rd_frac}
	cd rd_frac_${rd_frac}
	for c in `seq 1 1 $MAX_RUN_CLIENTS`
	do
	    echo "Exec $rd_frac $q $c" >> ${ROOT}/progress
	    echo "quorum=$q clients=$c"
	    mclast=$(($c + $c))
	    if [ "$mclast" -gt "24" ]; then
		mclast=24
	    fi
	    echo "mclast=$mclast"
	    config_clients=$(($c + 1)) # account for coordinator
	    mkdir c_$c
	    cd c_$c
	    echo "Running test" >> ${ROOT}/progress
	    echo '[meta]' > example.ini
	    echo "quorums=$q" >> example.ini
	    echo "clients=$config_clients" >> example.ini
	    cat ${ROOT}/example_base.ini >> example.ini
	    cp ${ROOT}/cluster.ini .
	    python ${ROOT}/../cyclone.git/utils-global/config_generator.py cluster.ini example.ini test_output
	    ${ROOT}/../cyclone.git/utils-global/deploy_configs.sh test_output /root/cyclone arch-h[2-24/2]
	    ${ROOT}/../cyclone.git/utils-global/deploy_driver.sh /root/cyclone arch-h[2-24/2]
	    clush -w arch-h[2-${mclast}/2] rm -f '/root/cyclone/client_log*'
	    clush -w arch-h[2-${mclast}/2] "echo export RBT_KEYS=$KEYS > /tmp/x"
	    clush -w arch-h[2-${mclast}/2] "echo export RBT_FRAC_READ=$rd_frac_float >> /tmp/x"
	    clush -w arch-h[2-${mclast}/2] "cat /root/cyclone/launch_client >> /tmp/x"
	    clush -w arch-h[2-${mclast}/2] "mv /tmp/x /root/cyclone/launch_client"
	    clush -w arch-h[2-${mclast}/2] /root/cyclone/exec_client.sh
	    echo "Sleeping for 2 min while test runs"
	    sleep 120 # run for 2 min
	    clush -w arch-h[2-${mclast}/2] killall -9 counter_driver
	    mc=2
	    last_c=$(($c - 1))
	    for mx in `seq 0 1 $last_c`
	    do
		scp arch-h${mc}:/root/cyclone/client_log${mx} client_log${mx}_${mc}
		mc=$(($mc + 2))
		if [ "$mc" -gt "24" ]; then
		    mc=2
		fi
	    done
	    echo "Complete $rd_frac $q $c" >> ${ROOT}/progress
	    cd ..
	done
	cd ..
    done
    cd preload
    for m in `seq 2 2 24`
    do
	scp arch-h${m}:/root/cyclone/server_log server_log_${m}
    done
    cd ..
    cd ..
done
${ROOT}/../killtest_arch_cluster