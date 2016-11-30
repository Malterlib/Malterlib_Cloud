R"-----(

echo PROVIDE KEY
cryptsetup --hash=plain --cipher=aes-xts-plain64 --offset=0 --key-file=- --key-size=512 open --type=plain $MibCloudApp_EncryptionStorage $MibCloudApp_DeviceName < /dev/stdin

if [[ "$MibCloudApp_EncryptionFileSystem" == "zfs" ]] ; then
	echo "536870912" > /sys/module/zfs/parameters/zfs_arc_max

	zpool import \
		-o cachefile=none \
		-d /dev/mapper \
		-N \
		$MibCloudApp_ZPoolName

	zfs set "mountpoint=$MibCloudApp_MountPoint" $MibCloudApp_ZPoolName
	zfs set "xattr=sa" $MibCloudApp_ZPoolName
	zfs mount $MibCloudApp_ZPoolName
elif [[ "$MibCloudApp_EncryptionFileSystem" == "ext4" || "$MibCloudApp_EncryptionFileSystem" == "xfs" ]] ; then
	mkdir -p "$MibCloudApp_MountPoint"
	mount /dev/mapper/$MibCloudApp_DeviceName "$MibCloudApp_MountPoint"
else
	echo Unknown file system $MibCloudApp_EncryptionFileSystem
	exit 1
fi

exit 0

)-----"
