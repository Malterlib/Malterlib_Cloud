// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	void CVersionManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CVersionManager>(mp_AppState.m_DistributionManager, this) > [](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLog(Error, "Failed to publish version manager {}", _Result.f_GetExceptionStr());
			}
		;
	}
}
