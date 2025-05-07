// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	void CCloudAPIManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CCloudAPIManager>(mp_AppState.m_DistributionManager, this) > [](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLog(Error, "Failed to publish cloud api manager interface: {}", _Result.f_GetExceptionStr());
			}
		;
	}
}
