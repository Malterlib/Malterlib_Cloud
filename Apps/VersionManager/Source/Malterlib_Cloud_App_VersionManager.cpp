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
		: CDistributedAppActor(CDistributedAppActor_Settings{"VersionManager", false}.f_AuditCategory("Malterlib/Cloud/VersionManager"))
	{
	}
	
	CVersionManagerDaemonActor::~CVersionManagerDaemonActor()
	{
	}

	TCContinuation<void> CVersionManagerDaemonActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCContinuation<void> Continuation;
		mp_pServer = fg_ConstructActor<CServer>(fg_Construct(self), mp_State);
		Continuation.f_SetResult();
		return Continuation;				
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_pServer)
		{
			DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Info, "Shutting down");
			
			mp_pServer->f_Destroy2() > [pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/VersionManager/Daemon, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
				}
			;
			mp_pServer = nullptr;
		}
		
		return pCanDestroy->m_Continuation;
	}
}
