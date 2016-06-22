R"-----(#!/bin/bash

set -e

if zpool status $MibCloudApp_ZPoolName 2>/dev/null 1>/dev/null ; then
	zpool export $MibCloudApp_ZPoolName 
fi

if cryptsetup status $MibCloudApp_DeviceName 2>/dev/null 1>/dev/null ; then
	cryptsetup close $MibCloudApp_DeviceName	
fi

exit 0

)-----"
