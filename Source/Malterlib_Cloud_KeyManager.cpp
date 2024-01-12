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
	}
	
	CKeyManager::~CKeyManager() = default;
}
