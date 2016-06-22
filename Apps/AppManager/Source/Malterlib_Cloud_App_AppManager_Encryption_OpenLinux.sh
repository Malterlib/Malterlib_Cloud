R"-----(#!/bin/bash

set -e

if zpool status $MibCloudApp_ZPoolName 2>/dev/null 1>/dev/null ; then
	zpool export $MibCloudApp_ZPoolName 
fi

if cryptsetup status $MibCloudApp_DeviceName 2>/dev/null 1>/dev/null ; then
	cryptsetup close $MibCloudApp_DeviceName	
fi

echo PROVIDE KEY
cryptsetup --hash=plain --cipher=aes-xts-plain64 --offset=0 --key-file=- --key-size=512 open --type=plain $MibCloudApp_EncryptionStorage $MibCloudApp_DeviceName < /dev/stdin

echo "536870912" > /sys/module/zfs/parameters/zfs_arc_max

zpool import \
	-o cachefile=none \
	-d /dev/mapper \
	-N \
	$MibCloudApp_ZPoolName

zfs set "mountpoint=$MibCloudApp_MountPoint" $MibCloudApp_ZPoolName
zfs set "xattr=sa" $MibCloudApp_ZPoolName
zfs mount $MibCloudApp_ZPoolName

exit 0

)-----"
