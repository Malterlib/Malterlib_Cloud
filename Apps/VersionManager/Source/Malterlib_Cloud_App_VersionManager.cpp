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
	CVersionManagerDaemonActor::CVersionManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"VersionManager"}.f_AuditCategory("Malterlib/Cloud/VersionManager"))
	{
	}
	
	CVersionManagerDaemonActor::~CVersionManagerDaemonActor()
	{
	}

	TCFuture<void> CVersionManagerDaemonActor::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		mp_pServer = fg_ConstructActor<CServer>(fg_Construct(self), mp_State);
		
		co_return {};
	}
	
	TCFuture<void> CVersionManagerDaemonActor::fp_StopApp()
	{	
		if (mp_pServer)
		{
			DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Info, "Shutting down");
			
			auto Result = co_await fg_Move(mp_pServer).f_Destroy().f_Wrap();
			if (!Result)
				DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Error, "Failed to shut down server: {}", Result.f_GetExceptionStr());
		}
		
		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_VersionManager()
	{
		return fg_Construct<NVersionManager::CVersionManagerDaemonActor>();
	}
}
