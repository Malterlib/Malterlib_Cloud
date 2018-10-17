R"-----(#!/bin/bash

set -e

if [ "$PlatformFamily" == "OSX" ]; then
	sysctl -w kern.maxfiles=$NumFiles
	sysctl -w kern.maxfilesperproc=$NumFilesPerProc
	sysctl -w kern.ipc.somaxconn=2048
	sysctl -w net.inet.ip.portrange.first=16384
	sysctl -w net.inet.ip.portrange.last=65535
	sysctl -w net.inet.ip.portrange.hifirst=16384
	sysctl -w net.inet.ip.portrange.hilast=65535

elif  [ "$PlatformFamily" == "Linux" ]; then
	sysctl -w fs.file-max=$NumFiles
	sysctl -w net.core.somaxconn=2048
	sysctl -w kernel.threads-max=$NumThreads
	sysctl -w kernel.pid_max=$NumPids
    sysctl -w vm.zone_reclaim_mode=0

	echo never > /sys/kernel/mm/transparent_hugepage/defrag
	echo never > /sys/kernel/mm/transparent_hugepage/enabled
elif  [ "$PlatformFamily" == "Windows" ]; then
	echo Not implemented
else
	echo Unknown platform, cannot setup OS
	exit 1
fi

exit 0

)-----"
