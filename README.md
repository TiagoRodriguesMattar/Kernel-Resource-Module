# Kernel-Resource-Module
Linux Kernel Module that shows some resource statistics about the system for the user (CPU usage percentage, some Memory stats and Disk Devices info). These statistics are updated in every 1 second.

Use the command make to compile the code. 

"sudo insmod krm.ko" to add our module. 

"cat /proc/krm_stats" to see the stats (or "watch -n 1 cat /proc/krm_stats").

"sudo rmmod krm" to remove our module.

*** "krm.c" and "Makefile" needs to be in the same directory. ***
