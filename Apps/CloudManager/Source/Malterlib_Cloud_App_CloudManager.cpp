// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"

namespace NMib::NCloud::NCloudManager
{
	CCloudManagerApp::CCloudManagerApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"CloudManager"})
	{
	}
	
	CCloudManagerApp::~CCloudManagerApp()
	{
	}

	TCFuture<void> CCloudManagerApp::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_pServer = fg_ConstructActor<CCloudManagerServer>(fg_Construct(self), mp_State);
		co_await mp_pServer(&CCloudManagerServer::f_Init);

		co_return {};
	}

	TCFuture<void> CCloudManagerApp::fp_StopApp()
	{	
		if (!mp_pServer)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/CloudManager/Daemon, Info, "Shutting down");

		auto DestroyResult = co_await mp_pServer->f_Destroy().f_Wrap();
		if (!DestroyResult)
			DMibLogWithCategory(Mib/Cloud/CloudManager/Daemon, Error, "Failed to shut down server: {}", DestroyResult.f_GetExceptionStr());

		mp_pServer = nullptr;

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_CloudManager()
	{
		return fg_Construct<NCloudManager::CCloudManagerApp>();
	}
}
