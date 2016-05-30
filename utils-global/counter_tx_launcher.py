def launch_cmds_startup():
    print("Configuring for counter tx application")


def launch_cmds_server_gen(f, q, r, m, quorums, replicas, clients):
    if q < (quorums - 1):
        cmd='counter_server '
        cmd=cmd + str(r) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients) + ' '
        cmd=cmd + 'config_server.ini config_client.ini &> server_log &\n'
    else:
        cmd='counter_coordinator '
        cmd=cmd + str(r) + ' '
        cmd=cmd + str(m) + ' '
        cmd=cmd + str(clients) + ' '
        cmd=cmd + str(quorums - 1) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + 'config_server config_client &> coord_log &\n'
    f.write(cmd)

def launch_cmds_preload_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m and c != (clients - 1):
        cmd='counter_loader '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients - 1) + ' 0 '
        cmd=cmd + str(quorums - 1) + ' '
        cmd=cmd + 'config_server config_client &> client_log' + str(c) + '\n'
        f.write(cmd)

def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m and c != (clients - 1):
        cmd='counter_coordinator_driver '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients - 1) + ' 0 '
        cmd=cmd + str(quorums - 1) + ' '
        cmd=cmd + 'config_server config_client &> client_tx_log' + str(c) + '&\n'
        f.write(cmd)
        
def killall_cmds_gen(f):
    f.write('killall -9 counter_server\n')
    f.write('killall -9 counter_loader\n')
    f.write('killall -9 counter_coordinator_driver\n')
    f.write('killall -9 counter_coordinator\n')
