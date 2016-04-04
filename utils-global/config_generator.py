import sys
import ConfigParser #Should be configparser in python v3
import os
import shutil

baseport=7000

def cond_abs_dir(name):
    if not os.path.exists(name):
        os.mkdir(name)

def cond_abs_rm(name):
    if os.path.exists(name):
        os.remove(name)



cluster=sys.argv[1]
input=sys.argv[2]
output=sys.argv[3]
config=ConfigParser.RawConfigParser()
config.read(input)
quorums=config.getint('meta', 'quorums')
replicas=config.getint('meta','replicas')
co_replicas=config.getint('meta','co_replicas')
clients=config.getint('meta','clients')

#read inactive list
inactive_list    = {}
if config.has_section('inactive'):
    inactive_cnt=config.getint('inactive','count')
    for i in range(0, inactive_cnt):
        mc=config.get('inactive','mc'+str(i))
        inactive_list[str(mc)]='yes'

raftpath=config.get('meta','raftpath') + ".cyclone"
filepath=config.get('meta','filepath') + ".cyclone"
coord_raftpath=config.get('meta','coord_raftpath') + ".cyclone"
coord_filepath=config.get('meta','coord_filepath') + ".cyclone"


logsize=config.getint('meta','logsize')
server_baseports = {}
client_baseports = {}

cond_abs_dir(output)

#load machine config
mc_config=ConfigParser.RawConfigParser()
mc_config.read(cluster)
machines=mc_config.getint('machines','count')

ports=2*max(replicas,co_replicas)*machines*(clients + 1)

# Generate server configs
for q in range(0, quorums):
    qstring='quorum' + str(q)
    server_baseports[str(q)] = baseport
    baseport = baseport + ports
    client_baseports[str(q)] = baseport
    baseport = baseport + ports
    config_name=output + '/' + 'config' + str(q) + '.ini'
    f=open(config_name, 'w')
    f.write('[storage]\n')
    f.write('raftpath=' + raftpath + '\n')
    f.write('logsize=' + str(logsize) + '\n')
    f.write('[quorum]\n')
    f.write('baseport=5000\n')
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
    f.write('server_baseport=' + str(server_baseports[str(q)]) + '\n')
    f.write('client_baseport=' + str(client_baseports[str(q)]) + '\n')
    f.write('filepath=' + str(filepath) + '\n')
    f.close()
    for r in range(0, replicas):
        rstring='mc' + str(r)
        m=mc_config.get('machines','addr' + config.get(qstring, rstring))
        dname=output + '/' + m
        cond_abs_dir(dname)
        cond_abs_rm(output + '/' + 'launch_servers.sh')
        cond_abs_rm(output + '/' + 'launch_inactive_servers.sh')
        shutil.copy(config_name,  dname + '/' + 'config_server.ini')

# Generate coordinator config
coord_baseport = baseport
baseport = baseport + ports
coord_client_baseport = baseport
baseport = baseport + ports
f=open(output + '/' + 'config_coord.ini', 'w')
f.write('[storage]\n')
f.write('raftpath=' + coord_raftpath + '\n')
f.write('logsize=' + str(logsize) + '\n')
f.write('[quorum]\n')
f.write('baseport=6000\n')
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
f.write('server_baseport='+ str(coord_baseport) + '\n')
f.write('client_baseport='+ str(coord_client_baseport) + '\n')
f.write('filepath=' + str(coord_filepath) + '\n')
f.close()
for r in range(0, co_replicas):
    qstring='coord'
    rstring='mc' + str(r)
    m=mc_config.get('machines','addr' + config.get(qstring, rstring))
    dname=output + '/' + m
    cond_abs_dir(dname)
    cond_abs_rm(output + '/' + 'launch_coord.sh')
    

# Generate server launch cmd
for q in range(0, quorums):
    for r in range(0, replicas):
        qstring='quorum' + str(q)
        rstring='mc' + str(r)
        mc_id = config.get(qstring, rstring)
        m=mc_config.get('machines','addr' + str(mc_id))
        dname=output + '/' + m
        if not str(mc_id) in inactive_list:
            f=open(dname + '/' + 'launch_servers','w')
        else:
            f=open(dname + '/' + 'launch_inactive_servers','w')
        cmd='./rbtree_map_server '
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
    dname=output + '/' + m
    f=open(dname + '/' + 'launch_coord','w')
    cmd='./rbtree_map_coordinator '
    cmd=cmd + str(r) + ' '
    cmd=cmd + str(co_replicas) + ' '
    cmd=cmd + str(clients) + ' '
    cmd=cmd + str(quorums) + ' '
    cmd=cmd + str(replicas) + ' '
    cmd=cmd + 'config_coord.ini config_coord_client.ini config config_client &> coord_log &\n'
    f.write(cmd)
    f.close()


#Generate client configs
for q in range(0, quorums):
    f=open(output + '/' + 'config_client' + str(q) + '.ini', 'w')
    f.write('[machines]\n')
    f.write('machines=' + str(machines) + '\n')
    for i in range(0, machines):
        addr=mc_config.get('machines','addr' + str(i))
        iface=mc_config.get('machines','iface' + str(i))
        f.write('addr' + str(i) + '=' + addr + '\n')
        f.write('iface' + str(i) + '=' + iface + '\n')
    f.write('[dispatch]\n')
    f.write('server_baseport=' + str(server_baseports[str(q)]) + '\n')
    f.write('client_baseport=' + str(client_baseports[str(q)]) + '\n')
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
f.write('server_baseport=' + str(coord_baseport) + '\n')
f.write('client_baseport=' + str(coord_client_baseport) + '\n')
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
            cmd='./rbtree_map_coordinator_driver '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients) + ' 0 '
            cmd=cmd + 'config_coord.ini '
            cmd=cmd + 'config_coord_client.ini '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config config_client &> client_tx_log' + str(c) + '&\n'
            f_tx_client.write(cmd)
            cmd='./rbtree_map_coordinator_load '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients) + ' 0 '
            cmd=cmd + 'config_coord.ini '
            cmd=cmd + 'config_coord_client.ini '
            f_preload.write(cmd)
            cmd='./rbtree_map_partitioned_driver '
            cmd=cmd + str(c) + ' '
            cmd=cmd + str(co_replicas) + ' '
            cmd=cmd + str(clients) + ' 0 '
            cmd=cmd + str(quorums) + ' '
            cmd=cmd + 'config config_client &> client_log' + str(c) + '&\n'
            f_driver.write(cmd)
    f_tx_client.close()
    f_preload.close()
    f_driver.close()





