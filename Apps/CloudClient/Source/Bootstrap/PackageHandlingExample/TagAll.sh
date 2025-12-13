#!/bin/bash

set -e 

RootDir="$PWD"

Applications=`"$RootDir/MalterlibCloud" --version-manager-list-applications --table-type tab-separated | sed 's/"//g'`

for Application in $Applications; do
	Version=`"$RootDir/MalterlibCloud" --version-manager-list-versions --table-type "tab-separated" "$Application" | tail -n 1 | cut "-d	" -f 2 | sed 's/"//g'`
	echo $Application $Version
	"$RootDir/MalterlibCloud" --version-manager-change-tags --application "$Application" --version "$Version" --add "[\"$1\"]"
done
