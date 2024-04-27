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
		if ! lsof -p $1 +r 1 2>&1 ; then
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

	Log "AppManager exited"
}

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
	WaitForPidToExit $PPID
	Log "Launching new daemon executable:" "$1" --daemon-restart
	"$1" --daemon-restart >> "$LogFile" 2>&1
	NewProcessID=$!
else
	Log "Unknow self update type: $2"
	exit 1
fi

Log "Successfully launched. New PID: $NewProcessID"

)-----"
