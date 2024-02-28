// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/DistributedActor>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib::NCloud
{
	CKeyManager::CKeyManager()
	{
		DMibPublishActorFunction(CKeyManager::f_RequestKey);
		DMibPublishActorFunction(CKeyManager::f_GetServerSyncInterface);
	}
	
	CKeyManager::~CKeyManager() = default;

	CKeyManagerServerSync::CKeyManagerServerSync()
	{
		DMibPublishActorFunction(CKeyManagerServerSync::f_ReadDatabase);
		DMibPublishActorFunction(CKeyManagerServerSync::f_CreateNewKeys);
		DMibPublishActorFunction(CKeyManagerServerSync::f_UseAvailableKey);
		DMibPublishActorFunction(CKeyManagerServerSync::f_PreCreateKeys);
		DMibPublishActorFunction(CKeyManagerServerSync::f_KeysVerifiedOnServers);
		DMibPublishActorFunction(CKeyManagerServerSync::f_RemoveVerifiedHosts);
	}
	
	CKeyManagerServerSync::~CKeyManagerServerSync() = default;
}
