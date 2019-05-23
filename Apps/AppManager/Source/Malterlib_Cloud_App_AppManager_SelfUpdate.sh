R"-----(#!/bin/bash

if [[ "$3" != "Recursive" ]]; then
	set -m
	"${BASH_SOURCE[0]}" "$1" "$2" Recursive </dev/null &>/dev/null &
	disown
	exit 0
fi

SysName=$(uname -s)

function WaitForPidToExit()
{
	if [[ $SysName ==  Darwin* ]] ; then
		lsof -p $1 +r 1 &>/dev/null
	elif [[ $SysName ==  Linux* ]] ; then
		tail --pid=$1 -f /dev/null
	else
		while kill -0 "$1"; do
            sleep 0.1
        done
	fi
}

if [[ "$2" == "DaemonStandalone" ]]; then
	WaitForPidToExit $AppManagerPID
	"$1" --daemon-run-standalane &
	disown
elif [[ "$2" == "DaemonDebug" ]]; then
	WaitForPidToExit $AppManagerPID
	"$1" --daemon-run-debug &
	disown
elif [[ "$2" == "Daemon" ]]; then
	WaitForPidToExit $PPID
	"$1" --daemon-restart
else
	echo "Unknow self update type"
	exit 1
fi

)-----"
