#!/bin/bash
echo "replicas throughput(Mop/sec) latency(us) network(Gbps)" > results.csv
for replicas in 1 2 3 5
do
    for clients in 1 20 50 100 150 180 200 250 300 350 400
    do
	if [ -f log_${replicas}_${clients} ] ; then
	    echo -n "$replicas," >> results.csv
	    d=$((60*$replicas))
	    cat log_${replicas}_${clients} | tail -n 50 | awk -v c=$clients -v dm=$d '{t = t + $7; l = l + $11} END {print c*t/(50*1000000) "," l/50 "," 8*dm*c*t/(50*1000000*1000)}' >> results.csv 
	fi
    done
done