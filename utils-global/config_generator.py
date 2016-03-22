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
raftpath=config.get('meta','raftpath')
filepath=config.get('meta','filepath')
coord_filepath=config.get('meta','coord_filepath')
logsize=config.getint('meta','logsize')
server_baseports = {}
client_baseports = {}

cond_abs_dir(output)

ports=2*max(replicas,co_replicas)*clients


#load machine config
mc_config=ConfigParser.RawConfigParser()
mc_config.read(cluster)

# Generate server configs
for q in range(0, quorums):
    server_baseports[str(q)] = baseport
    baseport = baseport + ports
    client_baseports[str(q)] = baseport
    baseport = baseport + ports
    for r in range(0, replicas):
        qstring='quorum' + str(q)
        rstring='mc' + str(r)
        m=mc_config.get('machines','addr' + config.get(qstring, rstring))
        dname=output + '/' + m
        cond_abs_dir(dname)
        cond_abs_rm(output + '/' + 'launch_servers.sh')
        f=open(dname + '/' + 'config_server.ini', 'w')
        f.write('[storage]\n')
        f.write('raftpath=' + raftpath + '\n')
        f.write('logsize=' + str(logsize) + '\n')
        f.write('[quorum]\n')
        f.write('baseport=5000\n')
        f.write('[machines]\n')
        f.write('machines='+str(replicas)+'\n')
        for mc in range(0, replicas):
            mc_id=config.getint(qstring, 'mc' + str(mc))
            f.write('addr'+ str(mc) + '=' + mc_config.get('machines', 'addr' + str(mc_id)) + '\n')
            f.write('iface'+ str(mc) + '=' + mc_config.get('machines', 'iface' + str(mc_id)) + '\n')
        f.write('[dispatch]\n')
        f.write('server_baseport=' + str(server_baseports[str(q)]) + '\n')
        f.write('client_baseport=' + str(client_baseports[str(q)]) + '\n')
        f.write('filepath=' + str(filepath) + '\n')
        f.close()

# Generate coordinator configs
coord_baseport = baseport
baseport = baseport + ports
coord_client_baseport = baseport
baseport = baseport + ports
for r in range(0, co_replicas):
    qstring='coord'
    rstring='mc' + str(r)
    m=mc_config.get('machines','addr' + config.get(qstring, rstring))
    dname=output + '/' + m
    cond_abs_dir(dname)
    cond_abs_rm(output + '/' + 'launch_coord.sh')
    f=open(dname + '/' + 'config_coord.ini', 'w')
    f.write('[storage]\n')
    f.write('raftpath=' + raftpath + '\n')
    f.write('logsize=' + str(logsize) + '\n')
    f.write('[quorum]\n')
    f.write('baseport=6000\n')
    f.write('[machines]\n')
    f.write('machines='+str(co_replicas)+'\n')
    for mc in range(0, co_replicas):
        mc_id=config.getint(qstring, 'mc' + str(mc))
        f.write('addr'+ str(mc) + '=' + mc_config.get('machines', 'addr' + str(mc_id)) + '\n')
        f.write('iface'+ str(mc) + '=' + mc_config.get('machines', 'iface' + str(mc_id)) + '\n')
    f.write('[dispatch]\n')
    f.write('server_baseport='+ str(coord_baseport) + '\n')
    baseport=baseport+ports
    f.write('client_baseport='+ str(coord_client_baseport) + '\n')
    baseport=baseport+ports
    f.write('filepath=' + str(coord_filepath) + '\n')
    f.close()
        



# Generate server launch cmd
for q in range(0, quorums):
    for r in range(0, replicas):
        qstring='quorum' + str(q)
        rstring='mc' + str(r)
        m=mc_config.get('machines','addr' + config.get(qstring, rstring))
        dname=output + '/' + m
        f=open(dname + '/' + 'launch_servers','w')
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
    cmd=cmd + 'config_coord.ini config_coord_client.ini &> coord_log &\n'
    f.write(cmd)
    f.close()



#Generate client configs
for q in range(0, quorums):
    f=open(output + '/' + 'config_client' + str(q) + '.ini', 'w')
    f.write('[machines]\n')
    machine_count=mc_config.getint('machines','count')
    f.write('machines=' + str(machine_count) + '\n')
    for i in range(0, machine_count):
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
for i in range(0, mc_config.getint('machines','count')):
    addr=mc_config.get('machines','addr' + str(i))
    iface=mc_config.get('machines','iface' + str(i))
    f.write('addr' + str(i) + '=' + addr + '\n')
    f.write('iface' + str(i) + '=' + iface + '\n')
f.write('[dispatch]\n')
f.write('server_baseport=' + str(coord_baseport) + '\n')
f.write('client_baseport=' + str(coord_client_baseport) + '\n')
f.close()
      
    
