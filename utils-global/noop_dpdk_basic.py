def launch_cmds_startup():
    print("Configuring for noop counter application")


def launch_cmds_server_gen(f, q, r, m, quorums, replicas, clients):
    cmd=''
    if os.environ.has_key('RBT_SLEEP_USEC'):
        cmd=cmd + 'RBT_SLEEP_USEC=' + os.environ.get('RBT_SLEEP_USEC') + ' '
    cmd=cmd + ' PMEM_IS_PMEM_FORCE=1 '
    cmd=cmd + 'counter_server '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(m) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + 'config_cluster.ini config_quorum.ini &> server_log &\n'
    f.write(cmd)

def launch_cmds_preload_gen(f, m, c, quorums, replicas, clients, machines):
    cmd=''


def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if m >= replicas:
        client_machines=machines-replicas
        if client_machines > clients:
            client_machines = clients
        clients_per_machine=clients/client_machines
        c_start = clients_per_machine*(m - replicas)
        c_stop  = c_start + clients_per_machine
        if m == replicas + client_machines - 1:
            cstop = clients
        if c == 0 and m < replicas + client_machines:
            cmd=''
            if os.environ.has_key('PAYLOAD'):
                cmd=cmd + 'PAYLOAD=' + os.environ.get('PAYLOAD') + ' '
            if os.environ.has_key('CC_TX'):
                cmd=cmd + 'CC_TX=' + os.environ.get('CC_TX') + ' '    
            cmd=cmd + 'counter_driver_noop_mt '
            cmd=cmd + str(c_start) + ' '
            cmd=cmd + str(c_stop) + ' '
            cmd=cmd + str(m) + ' '
            cmd=cmd + str(replicas) + ' '
            cmd=cmd + str(clients) + ' '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config_cluster.ini config_quorum &> client_log' + str(0) + '&\n'
            f.write(cmd)
        
def killall_cmds_gen(f):
    f.write('killall -9 counter_server\n')
    f.write('killall -9 counter_loader\n')
    f.write('killall -9 counter_driver\n')
    f.write('killall -9 counter_coordinator\n')
    f.write('killall -9 counter_driver_mt\n')
    f.write('killall -9 counter_driver_noop_mt\n')
