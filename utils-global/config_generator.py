import sys
import ConfigParser #Should be configparser in python v3
import os
import shutil
import imp

#Check cmdline
if len(sys.argv) != 5:
    print('Usage:' + sys.argv[0] + ' cluster-ini app-ini launch-py output-dir')
    sys.exit(-1)

#Static portmap
raft_baseport=5000
dispatcher_baseport=7000
service_ports=5 # assume a server binds to this many ports

#Utilities
def compute_server_baseport(quorum):
    return dispatcher_baseport + quorum*replicas*service_ports

def compute_raft_baseport(quorum):
    return raft_baseport + quorum*replicas*service_ports


def cond_abs_dir(name):
    if not os.path.exists(name):
        os.mkdir(name)

def cond_abs_rm(name):
    if os.path.exists(name):
        os.remove(name)


# Load up all the application settings

input=sys.argv[2]
execfile(sys.argv[3])
launch_cmds_startup()
output=sys.argv[4]
config=ConfigParser.RawConfigParser()
config.read(input)
quorums=config.getint('meta', 'quorums')
replicas=config.getint('meta','replicas')
co_replicas=config.getint('meta','co_replicas')
clients=config.getint('meta','clients')
raftpath=config.get('meta','raftpath') + ".cyclone"
filepath=config.get('meta','filepath') + ".cyclone"
coord_raftpath=config.get('meta','coord_raftpath') + ".cyclone"
coord_filepath=config.get('meta','coord_filepath') + ".cyclone"
logsize=config.getint('meta','logsize')
heapsize=config.getint('meta','heapsize')
coord_heapsize=config.getint('meta','coord_heapsize')

#read inactive list
inactive_list    = {}
if config.has_section('inactive'):
    inactive_cnt=config.getint('inactive','count')
    for i in range(0, inactive_cnt):
        mc=config.get('inactive','mc'+str(i))
        inactive_list[str(mc)]='yes'


#load machine config
cluster=sys.argv[1]
mc_config=ConfigParser.RawConfigParser()
mc_config.read(cluster)
machines=mc_config.getint('machines','count')

#More utilitites
def replica_mc(q, r):
    return config.getint('quorum' + str(q), 'mc' + str(r))

#Start with a clean slate
cond_abs_dir(output)

#Create per-machine directories and populate common stuff
for i in range(machines):
    dname=output + '/cyclone_' + str(i)
    cond_abs_dir(dname)
    f=open(dname + '/ip_address', 'w')
    m=mc_config.get('machines','addr' + str(i))
    f.write(m)
    f.close()
    f=open(dname + '/kill_all', 'w')
    killall_cmds_gen(f)
    f.close()
    f=open(dname + '/' + 'config_client.ini', 'w')
    f.write('[machines]\n')
    f.write('machines=' + str(machines) + '\n')
    for j in range(0, machines):
        addr=mc_config.get('machines','addr' + str(j))
        iface=mc_config.get('machines','iface' + str(j))
        f.write('addr' + str(j) + '=' + addr + '\n')
        f.write('iface' + str(j) + '=' + iface + '\n')
    f.close()
    for c in range(0, clients):
        f=open(dname + '/' + 'launch_preload', 'a')
        launch_cmds_preload_gen(f, i, c, quorums, replicas, clients, machines)
        f.close()
        f=open(dname + '/' + 'launch_clients', 'a')
        launch_cmds_client_gen(f, i, c, quorums, replicas, clients, machines)
        f.close()
    

# Generate server configs and launch cmds
for q in range(0, quorums):
    qstring='quorum' + str(q)
    config_name=output + '/' + 'config_server' + str(q) + '.ini'
    f=open(config_name, 'w')
    f.write('[storage]\n')
    f.write('raftpath=' + raftpath + '\n')
    f.write('logsize=' + str(logsize) + '\n')
    f.write('[quorum]\n')
    f.write('baseport=' + str(compute_raft_baseport(q)) + '\n')
    f.write('[machines]\n')
    f.write('machines='+str(replicas)+'\n')
    active_count=0
    for mc in range(0, replicas):
        mc_id=config.getint(qstring, 'mc' + str(mc))
        f.write('addr'+ str(mc) + '=' + mc_config.get('machines', 'addr' + str(mc_id)) + '\n')
        f.write('iface'+ str(mc) + '=' + mc_config.get('machines', 'iface' + str(mc_id)) + '\n')
        if not str(mc_id) in inactive_list:
            active_count = active_count + 1
    f.write('[active]\n')
    f.write('replicas=' + str(active_count)+'\n')
    index=0
    for mc in range(0, replicas):
        mc_id=config.getint(qstring, 'mc' + str(mc))
        if not str(mc_id) in inactive_list:
            f.write('entry'+ str(index) +'=' + str(mc) +'\n')
            index=index+1
    f.write('[dispatch]\n')
    f.write('server_baseport=' + str(compute_server_baseport(q)) + '\n')
    f.write('filepath=' + str(filepath) + '\n')
    f.write('heapsize=' + str(heapsize) + '\n')
    f.close()
    for r in range(0, replicas):
        mc=replica_mc(q, r)
        dname=output + '/cyclone_' + str(mc)
        if not str(mc) in inactive_list:
            f=open(dname + '/' + 'launch_servers','a')
        else:
            f=open(dname + '/' + 'launch_inactive_servers','a')
        shutil.copy(config_name,  dname + '/config_server.ini')
        launch_cmds_server_gen(f, q, r, mc, quorums, replicas, clients)
        f.close()

#Copy per-quorum server and client configs to all directories
for i in range(machines):
    dname=output + '/cyclone_' + str(i)
    for q in range(0, quorums):
        shutil.copy(output + '/config_server' + str(q) + '.ini', dname)
        shutil.copy(dname + '/config_client.ini', dname + '/config_client' + str(q) + '.ini')

sys.exit()
        
# Generate coordinator config
config_name=output + '/' + 'config_coord.ini'
f=open(config_name, 'w')
f.write('[storage]\n')
f.write('raftpath=' + coord_raftpath + '\n')
f.write('logsize=' + str(logsize) + '\n')
f.write('[quorum]\n')
f.write('baseport=' + str(compute_raft_baseport(quorums)) + '\n')
f.write('[machines]\n')
f.write('machines='+str(co_replicas)+'\n')
for mc in range(0, co_replicas):
    qstring='coord'
    mc_id=config.getint(qstring, 'mc' + str(mc))
    f.write('addr'+ str(mc) + '=' + mc_config.get('machines', 'addr' + str(mc_id)) + '\n')
    f.write('iface'+ str(mc) + '=' + mc_config.get('machines', 'iface' + str(mc_id)) + '\n')
f.write('[active]\n')
f.write('replicas=' + str(co_replicas)+'\n')
for mc in range(0, co_replicas):
    f.write('entry'+ str(mc) +'=' + str(mc) +'\n')
f.write('[dispatch]\n')
f.write('server_baseport='+ str(compute_server_baseport(quorums)) + '\n')
f.write('filepath=' + str(coord_filepath) + '\n')
f.write('heapsize=' + str(coord_heapsize) + '\n')
f.close()
for r in range(0, co_replicas):
    qstring='coord'
    rstring='mc' + str(r)
    m=mc_config.get('machines','addr' + config.get(qstring, rstring))
    dname=output + '/' + mc_directory(q, r)
    cond_abs_dir(dname)
    cond_abs_rm(output + '/' + 'launch_coord.sh')
    shutil.copy(config_name,  dname + '/' + 'config_coord.ini')
    f = open(dname + '/ip_address')
    f.write(m)
    f.close()
cond_abs_rm(config_name)

# Generate server launch cmd
for q in range(0, quorums):
    for r in range(0, replicas):
        qstring='quorum' + str(q)
        rstring='mc' + str(r)
        mc_id = config.get(qstring, rstring)
        m=mc_config.get('machines','addr' + str(mc_id))
        dname=output + '/' + mc_directory(q, r)
        cmd='./counter_server '
        cmd=cmd + str(r) + ' '
        cmd=cmd + str(replicas) + ' '
        cmd=cmd + str(clients) + ' '
        cmd=cmd + 'config_server.ini config_client.ini &> server_log &\n'
        f.write(cmd)
        f.close()

# Generate coord launch cmd
for r in range(0, co_replicas):
    qstring='coord'
    rstring='mc' + str(r)
    m=mc_config.get('machines','addr' + config.get(qstring, rstring))
    dname=output + '/' + mc_directory()
    f=open(dname + '/' + 'launch_coord','w')
    cmd='./counter_coordinator '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(co_replicas) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + str(quorums) + ' '
    cmd=cmd + str(replicas) + ' '
    cmd=cmd + 'config_coord.ini config_coord_client.ini config config_client &> coord_log &\n'
    f.write(cmd)
    f.close()


#Copy client configs
for q in range(0, quorums):
    for r in range(0, replicas):
        qstring='quorum' + str(q)
        rstring='mc' + str(r)
        m=mc_config.get('machines','addr' + config.get(qstring, rstring))
        dname=output + '/' + m
        shutil.copy(output + '/config_client' + str(q) + '.ini', dname + '/config_client.ini')

#Generate coord client configs
f=open(output + '/' + 'config_coord_client.ini', 'w')
f.write('[machines]\n')
f.write('machines=' + str(machines) + '\n')
for i in range(0, machines):
    addr=mc_config.get('machines','addr' + str(i))
    iface=mc_config.get('machines','iface' + str(i))
    f.write('addr' + str(i) + '=' + addr + '\n')
    f.write('iface' + str(i) + '=' + iface + '\n')
f.write('[dispatch]\n')
f.write('server_baseport=' + str(compute_server_baseport(quorums)) + '\n')
f.close()


# Generate tx client launch cmd
for m in range(0, machines):
    addr=mc_config.get('machines','addr' + str(m))
    dname=output + '/' + addr
    cond_abs_dir(dname)
    f_tx_client=open(dname + '/' + 'launch_tx_client','w')
    f_preload=open(dname + '/' + 'launch_preload','w')
    f_driver=open(dname + '/' + 'launch_client','w')
    for c in range(0, clients - 1):
        mc=c%machines
        if mc==m:
            cmd='./counter_coordinator_driver '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients) + ' 0 '
            cmd=cmd + 'config_coord.ini '
            cmd=cmd + 'config_coord_client.ini '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config config_client &> client_tx_log' + str(c) + '&\n'
            f_tx_client.write(cmd)
            cmd='./counter_loader '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients - 1) + ' 0 '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config config_client &> client_log' + str(c) + '\n'
            f_preload.write(cmd)
            cmd='./counter_driver '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients - 1) + ' 0 '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config config_client &> client_log' + str(c) + '&\n'
            f_driver.write(cmd)
    f_tx_client.close()
    f_preload.close()
    f_driver.close()





