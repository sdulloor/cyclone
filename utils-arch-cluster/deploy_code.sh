#!/bin/bash
GROUP="arch-h[4-30/2].dp.intel.com"
tar -zcvf cyclone.git.tgz cyclone.git
clush -w ${GROUP} mkdir -p /root/cyclone
clush -w ${GROUP} --copy config-code.sh --dest /root/cyclone
clush -w ${GROUP} --copy cyclone.git.tgz --dest /root/cyclone
clush -w ${GROUP} /root/cyclone/config-code.sh

