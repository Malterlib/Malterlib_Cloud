#!/bin/bash

set -e

RootDir="$PWD"
SourceDir="$( cd "$( dirname "${BASH_SOURCE[0]}" )" && pwd )"

for File in `find "$SourceDir" -name '*.tar.gz'`; do
	echo $File
	./MalterlibCloud --version-manager-upload-version "$File" --force
done
