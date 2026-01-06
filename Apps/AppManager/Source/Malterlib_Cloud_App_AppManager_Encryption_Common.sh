R"-----(#!/bin/bash

set -e

if [[ "$MibCloudApp_EncryptionFileSystem" == "zfs" ]] ; then
	if zpool status $MibCloudApp_ZPoolName 2>/dev/null 1>/dev/null ; then
		zpool export $MibCloudApp_ZPoolName
	fi
elif [[ "$MibCloudApp_EncryptionFileSystem" == "ext4" || "$MibCloudApp_EncryptionFileSystem" == "xfs" ]] ; then
	if mountpoint -x -- "/dev/mapper/$MibCloudApp_DeviceName" 2>/dev/null 1>/dev/null ; then
		umount "/dev/mapper/$MibCloudApp_DeviceName" || true
	fi
else
	echo Unknown file system $MibCloudApp_EncryptionFileSystem
	exit 1
fi

if cryptsetup status $MibCloudApp_DeviceName 2>/dev/null 1>/dev/null ; then
	cryptsetup close $MibCloudApp_DeviceName
fi

)-----"
