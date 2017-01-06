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
clients=config.getint('meta','clients')
raftpath=config.get('meta','raftpath') + ".cyclone"
filepath=config.get('meta','filepath') + ".cyclone"
logsize=config.getint('meta','logsize')
heapsize=config.getint('meta','heapsize')
ports=config.getint('meta', 'ports')

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
    m=mc_config.get('machines','config' + str(i))
    f.write(m)
    f.close()
    f=open(dname + '/kill_all', 'w')
    killall_cmds_gen(f)
    f.close()
    for c in range(0, clients):
        f=open(dname + '/' + 'launch_preload', 'a')
        launch_cmds_preload_gen(f, i, c, quorums, replicas, clients, machines, ports)
        f.close()
        f=open(dname + '/' + 'launch_clients', 'a')
        launch_cmds_client_gen(f, i, c, quorums, replicas, clients, machines, ports)
        f.close()
    

# Generate server configs and launch cmds
for q in range(0, quorums):
    qstring='quorum' + str(q)
    config_name=output + '/' + 'config_quorum' + str(q) + '.ini'
    f=open(config_name, 'w')
    f.write('[storage]\n')
    f.write('raftpath=' + raftpath + '\n')
    f.write('logsize=' + str(logsize) + '\n')
    f.write('[quorum]\n')
    f.write('baseport=' + str(compute_raft_baseport(q)) + '\n')
    f.write('replicas='+str(replicas)+'\n')
    active_count=0
    for mc in range(0, replicas):
        mc_id=config.getint(qstring, 'mc' + str(mc))
        f.write('replica'+ str(mc) + '=' + str(mc_id) + '\n')
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
        shutil.copy(config_name,  dname + '/config_quorum.ini')
        shutil.copy(sys.argv[1],  dname + '/config_cluster.ini')
        if os.environ.has_key('CLIENT_ASSIST'):
            f.write('export CLIENT_ASSIST=1\n')
        launch_cmds_server_gen(f, q, r, mc, quorums, replicas, clients, ports)
        f.close()

#Copy configs to all directories
for i in range(machines):
    dname=output + '/cyclone_' + str(i)
    for q in range(0, quorums):
        shutil.copy(output + '/config_quorum' + str(q) + '.ini', dname)
        shutil.copy(sys.argv[1],  dname + '/config_cluster.ini')

