// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>

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

	TCFuture<void> CCloudManagerApp::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		mp_Server = fg_ConstructActor<CCloudManagerServer>(fg_Construct(self), mp_State);
		co_await mp_Server(&CCloudManagerServer::f_Init);

		co_return {};
	}

	TCFuture<void> CCloudManagerApp::fp_StopApp()
	{	
		if (!mp_Server)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/CloudManager/Daemon, Info, "Shutting down");

		auto DestroyResult = co_await fg_Move(mp_Server).f_Destroy().f_Wrap();
		if (!DestroyResult)
			DMibLogWithCategory(Mib/Cloud/CloudManager/Daemon, Error, "Failed to shut down server: {}", DestroyResult.f_GetExceptionStr());

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
