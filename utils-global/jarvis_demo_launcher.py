def launch_cmds_startup():
    print("Configuring for Jarvis demo application")


def launch_cmds_server_gen(f, q, r, m, quorums, replicas, clients):
    cmd='jarvis_demo_server '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(replicas) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + 'config_server.ini config_client.ini &> server_log &\n'
    f.write(cmd)

def launch_cmds_preload_gen(f, m, c, quorums, replicas, clients, machines):
    if c == machines:
        cmd='rm -rf jarvis_demo_graph;jarvis_demo_setup'
        f.write(cmd)

def launch_cmds_client_gen(f, m, c, quorums, replicas, clients, machines):
    if c % machines == m and c != (clients - 1):
        cmd='jarvis_demo_client '
        cmd=cmd + str(c) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients) + ' 0 '
        cmd=cmd + 'config_server config_client &> client_log' + str(c) + '&\n'
        f.write(cmd)
        
def killall_cmds_gen(f):
    f.write('killall -9 jarvis_demo_server\n')
    f.write('killall -9 jarvis_demo_client\n')
    f.write('killall -9 jarvis_demo_setup\n')
