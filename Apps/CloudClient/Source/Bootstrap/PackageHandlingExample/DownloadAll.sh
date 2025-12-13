#!/bin/bash

set -e

RootDir="$PWD"

rm -rf Repo
mkdir -p Repo
pushd Repo > /dev/null

mkdir -p MalterlibCloud
pushd MalterlibCloud > /dev/null

Applications=`"$RootDir/MalterlibCloud" --version-manager-list-applications --table-type tab-separated --no-color | sort | uniq | cut -d$'\t' -f 2 | xargs`

for Application in $Applications; do
	if [[ $Application = *Favro* ]]; then
		continue
	fi
	if [[ $Application = BackupManager ]]; then
		continue
	fi
	if [[ $Application = BackupManager ]]; then
		continue
	fi
	if [[ $Application = CloudAPIManager ]]; then
		continue
	fi
	if [[ $Application = BigHead ]]; then
		continue
	fi
	if [[ $Application = AOTool ]]; then
		continue
	fi
	if [[ $Application = MTool ]]; then
		continue
	fi
	if [[ $Application = MongoManager ]]; then
		continue
	fi

	echo $Application
	mkdir -p "$Application"
	pushd "$Application" > /dev/null

	Version=`"$RootDir/MalterlibCloud" --version-manager-list-versions "$Application" --table-type tab-separated --no-color | grep -e 'master/' | tail -n 1 | cut -d$'\t' -f 2 | xargs`
	Platforms=`"$RootDir/MalterlibCloud" --version-manager-list-versions "$Application" --table-type tab-separated --no-color | grep -e "$Version" | cut -d$'\t' -f 3 | xargs`

	for Platform in $Platforms; do
		mkdir -p "$Platform"
		pushd "$Platform" > /dev/null
		echo "$Application $Version $Platform"
		"$RootDir/MalterlibCloud" --version-manager-download-version --application "$Application" --platform "$Platform" --version "$Version" "$PWD" &
		echo ""

		popd > /dev/null
	done

	popd > /dev/null
done

popd > /dev/null

wait

cp ../UploadAll.sh MalterlibCloud
cp ../TagAll.sh MalterlibCloud

zip -r -q MalterlibCloud.zip MalterlibCloud

popd > /dev/null

