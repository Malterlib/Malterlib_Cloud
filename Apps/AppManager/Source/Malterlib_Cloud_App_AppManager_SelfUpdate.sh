R"-----(#!/bin/bash

if [[ "$3" != "Recursive" ]]; then
	set -m
	"${BASH_SOURCE[0]}" "$1" "$2" Recursive </dev/null &>/dev/null &
	disown
	exit 0
fi

if [[ "$2" == "DaemonStandalone" ]]; then
	sleep 5
	"$1" --daemon-run-standalane &
	disown
elif [[ "$2" == "DaemonDebug" ]]; then
	sleep 5
	"$1" --daemon-run-debug &
	disown
elif [[ "$2" == "Daemon" ]]; then
	# Leave time for script to exit and disown
	sleep 0.1
	"$1" --daemon-restart
else
	echo "Unknow self update type"
	exit 1
fi

)-----"
