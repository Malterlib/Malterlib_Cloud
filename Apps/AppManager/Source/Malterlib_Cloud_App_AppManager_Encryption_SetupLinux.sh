#!/bin/bash

echo PROVIDE KEY
cryptsetup --hash=plain --cipher=aes-xts-plain64 --offset=0 --key-file=- --key-size=512 open --type=plain $MibCloudApp_EncryptionStorage $MibCloudApp_DeviceName < /dev/stdin

if [[ "$MibCloudApp_EncryptionFileSystem" == "zfs" ]] ; then
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
elif [[ "$MibCloudApp_EncryptionFileSystem" == "ext4" || "$MibCloudApp_EncryptionFileSystem" == "xfs" ]] ; then
	mkfs.$MibCloudApp_EncryptionFileSystem /dev/mapper/$MibCloudApp_DeviceName
	mkdir -p "$MibCloudApp_MountPoint"
	mount /dev/mapper/$MibCloudApp_DeviceName "$MibCloudApp_MountPoint" -o noatime
else
	echo Unknown file system $MibCloudApp_EncryptionFileSystem
	exit 1
fi

exit 0
