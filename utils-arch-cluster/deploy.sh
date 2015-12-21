#!/bin/bash
GROUP="arch-h[2-30/2].dp.intel.com"

clush -w ${GROUP} mkdir -p /root/cyclone
clush -w ${GROUP} --copy config-machines.sh --dest /root/cyclone
clush -w ${GROUP} /root/cyclone/config-machines.sh

