// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"
#include "Malterlib_Cloud_App_SecretsManager_ServerController.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CSecretsManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"SecretsManager", false})
	{
	}
	
	CSecretsManagerDaemonActor::~CSecretsManagerDaemonActor()
	{
	}

	TCContinuation<void> CSecretsManagerDaemonActor::fp_StartApp(CEJSON const &_Params)
	{
		mp_pServerController = fg_ConstructActor<CServerController>(fg_Construct(self), self, mp_State);
		return fg_Explicit();
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_pServerController)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Shutting down server");
			
			mp_pServerController->f_Destroy() > [this, pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
					mp_pServerController = nullptr;
				}
			;
		}
		
		return pCanDestroy->m_Continuation;
	}

#if DMibConfig_Tests_Enable
	TCContinuation<CEJSON> CSecretsManagerDaemonActor::fp_Test_Command(CStr const &_Command, CEJSON const &_Params)
	{
		// This function is used to provoke some special cases during testing
		if (!mp_pServerController)
			DMibError("No server controller");

		return DMibCallActor(mp_pServerController, CServerController::f_Test_Command, _Command, _Params);
	}
#endif
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_SecretsManager()
	{
		return fg_Construct<NSecretsManager::CSecretsManagerDaemonActor>();
	}
}
