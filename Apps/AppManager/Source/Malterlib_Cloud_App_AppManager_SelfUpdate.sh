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
	"${BASH_SOURCE[0]}" "$1" "$2" Recursive </dev/null &>/dev/null &
	disown
	exit 0
fi

Log "Launched recursive"

SysName=$(uname -s)

function WaitForPidToExit()
{
	Log "Waiting for AppManager to exit"
	if [[ $SysName ==  Darwin* ]] ; then
		lsof -p $1 +r 1 &>/dev/null
	elif [[ $SysName ==  Linux* ]] ; then
		tail --pid=$1 -f /dev/null
	else
		while kill -0 "$1"; do
            sleep 0.1
        done
	fi
	Log "AppMangare exited"
}

if [[ "$2" == "DaemonStandalone" ]]; then
	WaitForPidToExit $AppManagerPID
	Log "Launching new standalone executable"
	"$1" --daemon-run-standalone >> "$LogFile" 2>&1 &
	disown
elif [[ "$2" == "DaemonDebug" ]]; then
	WaitForPidToExit $AppManagerPID
	Log "Launching new daemon debug executable"
	"$1" --daemon-run-debug >> "$LogFile" 2>&1 &
	disown
elif [[ "$2" == "Daemon" ]]; then
	WaitForPidToExit $PPID
	Log "Launching new daemon executable"
	"$1" --daemon-restart >> "$LogFile" 2>&1
else
	Log "Unknow self update type: $2"
	exit 1
fi

Log "Successfully launched"

)-----"
