#!/bin/bash
echo "replicas,payload,latency(us),throughput(Mop/sec),network(Gbps)" > results.csv
for replicas in 1 2 3
do
    for clients in 1 20 50 100 150 180 200 250 300 350 400
    do
	for payload in 0 190 512
	do
	    if [ -f client_log_${replicas}_${clients}_${payload} ]; then
		echo "Processing $replicas $clients $payload"
		echo -n "$replicas,$payload," >> results.csv
		d=$(($payload*$replicas + 60*$replicas))
		cat client_log_${replicas}_${clients}_${payload} | grep LOAD | grep -v PAYLOAD | tail -n 50 | awk '{l = l + $11;s=s+1} END {print l/s ","}' | xargs echo -n >> results.csv 
		cat server_log_${replicas}_${clients}_${payload} | grep 'Replication rate' | awk -v dm=$d '{s=s+1;t=t+$8} END{x=(t/s)*4*dm*8/1000;if(x>10) x=10;print t/s "," x}' >> results.csv
	    fi
	done
    done
done
