// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CSecretsManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"SecretsManager", false})
	{
	}
	
	CSecretsManagerDaemonActor::~CSecretsManagerDaemonActor()
	{
	}

	TCContinuation<void> CSecretsManagerDaemonActor::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCContinuation<void> Continuation;
		mp_pServer = fg_ConstructActor<CServer>(fg_Construct(self), mp_State);
		Continuation.f_SetResult();
		return Continuation;				
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_pServer)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager/Daemon, Info, "Shutting down");
			
			mp_pServer->f_Destroy() > [pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/SecretsManager/Daemon, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
				}
			;
			mp_pServer = nullptr;
		}
		
		return pCanDestroy->m_Continuation;
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_SecretsManager()
	{
		return fg_Construct<NSecretsManager::CSecretsManagerDaemonActor>();
	}
}
