#!/bin/bash
GROUP='arch-h[2-30/2].dp.intel.com'

clush -w ${GROUP} killall -9 counter_server
clush -w ${GROUP} killall -9 counter_coordinator
clush -w ${GROUP} killall -9 counter_coordinator_driver
clush -w ${GROUP} killall -9 counter_driver
clush -w ${GROUP} killall -9 counter_loader

