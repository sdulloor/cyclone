def launch_cmds_startup():
    print("Configuring for counter application")


def launch_cmds_server_gen(f, q, r, m, quorums, replicas, clients):
    cmd=''
    if os.environ.has_key('RBT_SLEEP_USEC'):
        cmd=cmd + 'RBT_SLEEP_USEC=' + os.environ.get('RBT_SLEEP_USEC') + ' '
    cmd=cmd + 'counter_server '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(replicas) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + 'config_server.ini config_client.ini &> server_log &\n'
    f.write(cmd)

def launch_cmds_preload_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m:
        cmd=''
        if os.environ.has_key('RBT_KEYS'):
            cmd=cmd + 'RBT_KEYS=' + os.environ.get('RBT_KEYS') + ' '
        cmd=cmd + 'counter_loader '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(m) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients) + ' '
        cmd=cmd + str(quorums) + ' '
        cmd=cmd + 'config_server config_client &> client_log' + str(c) + '\n'
        f.write(cmd)

def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m:
        cmd=''
        if os.environ.has_key('RBT_KEYS'):
            cmd=cmd + 'RBT_KEYS=' + os.environ.get('RBT_KEYS') + ' '
        if os.environ.has_key('RBT_FRAC_READ'):   
            cmd=cmd + 'RBT_FRAC_READ=' + os.environ.get('RBT_FRAC_READ') + ' '
        cmd=cmd + 'counter_driver '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(m) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients) + ' '
        cmd=cmd + str(quorums) + ' '
        cmd=cmd + 'config_server config_client &> client_log' + str(c) + '&\n'
        f.write(cmd)
        
def killall_cmds_gen(f):
    f.write('killall -9 counter_server\n')
    f.write('killall -9 counter_loader\n')
    f.write('killall -9 counter_driver\n')
    f.write('killall -9 counter_coordinator\n')