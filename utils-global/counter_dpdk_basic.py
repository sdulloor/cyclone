def launch_cmds_startup():
    print("Configuring for counter application")


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


def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if m > 2 and m < 7:
        c_start = 9*(m - 3)
        c_stop  = c_start + 9
        if c == 0:
            cmd=''
            cmd=cmd + 'counter_driver_noop_mt '
            cmd=cmd + str(c_start) + ' '
            cmd=cmd + str(c_stop) + ' '
            cmd=cmd + str(m) + ' '
            cmd=cmd + str(replicas) + ' '
            cmd=cmd + str(clients) + ' '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config_server config_client &> client_log' + str(0) + '&\n'
            f.write(cmd)
        
def killall_cmds_gen(f):
    f.write('killall -9 counter_server\n')
    f.write('killall -9 counter_loader\n')
    f.write('killall -9 counter_driver\n')
    f.write('killall -9 counter_coordinator\n')
    f.write('killall -9 counter_driver_mt\n')
