#!/bin/bash

set -e

ReturnValue=0
function ReportError()
{
	echo "WARNING: $1" >&2
	ReturnValue=2
}

if [ "$PlatformFamily" == "macOS" ]; then
	sysctl -w kern.maxfiles=$NumFiles || ReportError "Failed to set kern.maxfiles"
	sysctl -w kern.maxfilesperproc=$NumFilesPerProc || ReportError "Failed to set kern.maxfilesperproc"
	sysctl -w kern.ipc.somaxconn=2048 || ReportError "Failed to set kern.ipc.somaxconn"
	sysctl -w net.inet.ip.portrange.first=16384 || ReportError "Failed to set net.inet.ip.portrange.first"
	sysctl -w net.inet.ip.portrange.last=65535 || ReportError "Failed to set net.inet.ip.portrange.last"
	sysctl -w net.inet.ip.portrange.hifirst=16384 || ReportError "Failed to set net.inet.ip.portrange.hifirst"
	sysctl -w net.inet.ip.portrange.hilast=65535 || ReportError "Failed to set net.inet.ip.portrange.hilast"

elif  [ "$PlatformFamily" == "Linux" ]; then
	sysctl -w fs.file-max=$NumFiles || ReportError "Failed to set fs.file-max"
	sysctl -w net.core.somaxconn=2048 || ReportError "Failed to set net.core.somaxconn"
	sysctl -w kernel.threads-max=$NumThreads || ReportError "Failed to set kernel.threads-max"
	sysctl -w kernel.pid_max=$NumPids || ReportError "Failed to set kernel.pid_max"
	sysctl -w vm.max_map_count=$MaxMapCount || ReportError "Failed to set vm.max_map_count"
    sysctl -w vm.zone_reclaim_mode=0 || ReportError "Failed to set vm.zone_reclaim_mode"

	(echo never > /sys/kernel/mm/transparent_hugepage/defrag) || ReportError "Failed to disable transparent_hugepage/defrag"
	(echo never > /sys/kernel/mm/transparent_hugepage/enabled) || ReportError "Failed to disable transparent_hugepage/enabled"
elif  [ "$PlatformFamily" == "Windows" ]; then
	echo Not implemented
else
	echo Unknown platform, cannot setup OS
	exit 1
fi

exit $ReturnValue
