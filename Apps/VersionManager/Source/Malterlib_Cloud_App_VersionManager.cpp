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
	CVersionManagerDaemonActor::CVersionManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"VersionManager"}.f_AuditCategory("Malterlib/Cloud/VersionManager"))
	{
	}
	
	CVersionManagerDaemonActor::~CVersionManagerDaemonActor()
	{
	}

	TCFuture<void> CVersionManagerDaemonActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCPromise<void> Promise;
		mp_pServer = fg_ConstructActor<CServer>(fg_Construct(self), mp_State);
		Promise.f_SetResult();
		return Promise.f_MoveFuture();				
	}
	
	TCFuture<void> CVersionManagerDaemonActor::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_pServer)
		{
			DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Info, "Shutting down");
			
			mp_pServer->f_Destroy() > [pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
				}
			;
			mp_pServer = nullptr;
		}
		
		return pCanDestroy->f_Future();
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_VersionManager()
	{
		return fg_Construct<NVersionManager::CVersionManagerDaemonActor>();
	}
}
