#!/bin/bash
GROUP="sal-sdv[1-4]"

clush -w ${GROUP} --copy config-tests.sh --dest /data/devel/cyclone/
clush -w ${GROUP} /data/devel/cyclone/config-tests.sh
