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

Options=
if [ "$MibCloudApp_ForceOverwrite" == "1" ] ; then
	Options=-f
fi

zpool create \
	$Options \
	-o cachefile=none \
	-o ashift=12 \
	-O xattr=sa \
	-O mountpoint=$MibCloudApp_MountPoint \
	$MibCloudApp_ZPoolName \
	/dev/mapper/$MibCloudApp_DeviceName

exit 0

)-----"
