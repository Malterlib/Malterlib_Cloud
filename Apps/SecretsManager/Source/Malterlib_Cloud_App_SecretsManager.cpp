// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"
#include "Malterlib_Cloud_App_SecretsManager_ServerController.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CSecretsManagerDaemonActor()
		: CDistributedAppActor(CDistributedAppActor_Settings{"SecretsManager"})
	{
	}
	
	CSecretsManagerDaemonActor::~CSecretsManagerDaemonActor()
	{
	}

	TCFuture<void> CSecretsManagerDaemonActor::fp_StartApp(CEJSONSorted const _Params)
	{
		mp_pServerController = fg_ConstructActor<CServerController>(fg_Construct(self), self, mp_State);
		co_await mp_pServerController(&CServerController::f_Init);
		co_return {};
	}
	
	TCFuture<void> CSecretsManagerDaemonActor::fp_StopApp()
	{	
		if (mp_pServerController)
		{
			DMibLogWithCategory(Mib/Cloud/SecretsManager, Info, "Shutting down server");
			
			auto Result = co_await fg_TempCopy(mp_pServerController).f_Destroy().f_Wrap();
			if (!Result)
				DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to shut down server: {}", Result.f_GetExceptionStr());
		}
		
		co_return {};
	}

#if DMibConfig_Tests_Enable
	TCFuture<CEJSONSorted> CSecretsManagerDaemonActor::fp_Test_Command(CStr _Command, CEJSONSorted const _Params)
	{
		// This function is used to provoke some special cases during testing
		if (!mp_pServerController)
			co_return DMibErrorInstance("No server controller");

		co_return co_await mp_pServerController.f_CallActor(&CServerController::f_Test_Command)(fg_Move(_Command), fg_Move(fg_RemoveQualifiers(_Params)));
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
