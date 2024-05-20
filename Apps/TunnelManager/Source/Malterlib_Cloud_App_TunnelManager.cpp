// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_TunnelManager.h"
#include "Malterlib_Cloud_App_TunnelManager_Internal.h"

namespace NMib::NCloud::NTunnelManager
{
	CTunnelManagerApp::CTunnelManagerApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"TunnelManager"})
	{
	}
	
	CTunnelManagerApp::~CTunnelManagerApp()
	{
	}

	TCFuture<void> CTunnelManagerApp::fp_StartApp(NEncoding::CEJSONSorted const &_Params)
	{
		mp_Server = fg_ConstructActor<CTunnelManagerServer>(fg_Construct(self), mp_State);
		co_await mp_Server(&CTunnelManagerServer::f_Init);

		co_return {};
	}

	TCFuture<void> CTunnelManagerApp::fp_StopApp()
	{	
		if (!mp_Server)
			co_return {};

		DMibLogWithCategory(Mib/Cloud/TunnelManager/Daemon, Info, "Shutting down");

		auto DestroyResult = co_await fg_Move(mp_Server).f_Destroy().f_Wrap();
		if (!DestroyResult)
			DMibLogWithCategory(Mib/Cloud/TunnelManager/Daemon, Error, "Failed to shut down server: {}", DestroyResult.f_GetExceptionStr());

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_TunnelManager()
	{
		return fg_Construct<NTunnelManager::CTunnelManagerApp>();
	}
}
