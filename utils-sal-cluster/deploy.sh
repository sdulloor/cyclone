#!/bin/bash
GROUP="sal-sdv[2-4]"

clush -w ${GROUP} mkdir -p /data/devel/cyclone
clush -w ${GROUP} --copy config-machines.sh --dest /data/devel/cyclone
clush -w ${GROUP} /data/devel/cyclone/config-machines.sh

