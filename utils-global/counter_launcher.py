def launch_cmds_startup():
    print("Configuring for counter application")


def launch_cmds_server_gen(f, q, r, quorums, replicas, clients):
    cmd='counter_server '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(replicas) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + 'config_server.ini config_client.ini &> server_log &\n'
    f.write(cmd)

def launch_cmds_preload_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m:
        cmd='counter_loader '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients - 1) + ' 0 '
        cmd=cmd + str(quorums) + ' '
        cmd=cmd + 'config config_client &> client_log' + str(c) + '\n'
        f.write(cmd)

def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m:
        cmd='counter_driver '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients - 1) + ' 0 '
        cmd=cmd + str(quorums) + ' '
        cmd=cmd + 'config config_client &> client_log' + str(c) + '&\n'
        f.write(cmd)
        
