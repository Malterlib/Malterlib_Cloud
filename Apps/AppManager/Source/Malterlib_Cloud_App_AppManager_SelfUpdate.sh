R"-----(#!/bin/bash

set -e

LogFile="$PWD/Log/AppManagerSelfUpdate.log"
function Log()
{
	echo "`date +"%Y-%m-%d %H:%M:%S"`" $$ "$@" >> "$LogFile"
}

function ErrorReport()
{
	Log "Error on line $(caller)"
}

trap ErrorReport ERR

if [[ "$3" != "Recursive" ]]; then
	Log "Launching recursive"
	set -m
	export ParentPID=$$
	"${BASH_SOURCE[0]}" "$1" "$2" Recursive </dev/null >> "$LogFile" 2>&1 &
	disown
	exit 0
fi

Log "Launched recursive"

# Support only cgroups V2 unified and Hybrid modes
CGroupRoot="/sys/fs/cgroup/cgroup.procs"
if ! [ -f "$CGroupRoot" ]; then
	CGroupRoot="/sys/fs/cgroup/unified/cgroup.procs"
fi 

if [ -f "/proc/self/cgroup" ] && [ -f "$CGroupRoot" ]; then
	# Break out of systemd cgroup so we are not killed when daemon stops
	if cat /proc/self/cgroup | grep -q '/system\.slice/'; then
		Log "Moving process to root CGroup: $CGroupRoot"
		echo $$ > "$CGroupRoot"
	fi
fi

SysName=$(uname -s)

function WaitForPidToExit()
{
	Log "Waiting for $1 to exit"

	if [[ $SysName ==  Darwin* ]] ; then
		if ! lsof -p $1 +r 1 > /dev/null ; then
			echo lsof exited with error
		fi
	elif [[ $SysName ==  Linux* ]] ; then
		if ! tail --pid=$1 -f /dev/null ; then
			echo tail --pid exited with error
		fi
	else
		while kill -0 "$1"; do
            sleep 0.1
        done
	fi

	Log "$1 exited"
}

WaitForPidToExit $ParentPID

eval "ExtraLaunchParamsArray=($ExtraLaunchParams)"

if [[ "$2" == "DaemonStandalone" ]]; then
	WaitForPidToExit $AppManagerPID
	Log "Launching new standalone executable:" "$1" --daemon-run-standalone "${ExtraLaunchParamsArray[@]}"
	"$1" --daemon-run-standalone "${ExtraLaunchParamsArray[@]}" >> "$LogFile" 2>&1 &
	NewProcessID=$!
	disown
elif [[ "$2" == "DaemonDebug" ]]; then
	WaitForPidToExit $AppManagerPID
	Log "Launching new daemon debug executable:" "$1" --daemon-run-debug "${ExtraLaunchParamsArray[@]}"
	"$1" --daemon-run-debug "${ExtraLaunchParamsArray[@]}" >> "$LogFile" 2>&1 &
	NewProcessID=$!
	disown
elif [[ "$2" == "Daemon" ]]; then
	Log "Launching new daemon executable:" "$1" --daemon-restart
	"$1" --daemon-restart >> "$LogFile" 2>&1
	NewProcessID=$!
else
	Log "Unknown self update type: $2"
	exit 1
fi

Log "Successfully launched. New PID: $NewProcessID"

)-----"
