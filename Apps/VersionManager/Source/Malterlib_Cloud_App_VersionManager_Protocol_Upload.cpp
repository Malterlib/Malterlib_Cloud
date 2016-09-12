// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	auto CVersionManagerDaemonActor::CServer::fp_Protocol_UploadVersion(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CStartUploadVersion &&_Params)
		-> TCContinuation<CVersionManager::CStartUploadVersion::CResult> 
	{
		return {};
	}
}
