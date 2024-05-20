// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_TunnelManager_Internal.h"
#include "Malterlib_Cloud_App_TunnelManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Process/Platform>

namespace NMib::NCloud::NTunnelManager
{
	CTunnelManagerServer::CTunnelManagerServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
	}

	CTunnelManagerServer::~CTunnelManagerServer()
	{
	}

	TCFuture<void> CTunnelManagerServer::f_Init()
	{
		co_await (fp_Publish() % "Failed to publish");

		co_return {};
	}

	TCFuture<void> CTunnelManagerServer::fp_Destroy()
	{
		TCActorResultVector<void> Destroys;

		//mp_ProtocolInterface.f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetResults();

		co_return {};
	}

	TCFuture<void> CTunnelManagerServer::fp_SetupPermissions()
	{
		TCSet<CStr> Permissions
			{
				"TunnelManager/Test"
			}
		;
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();

		TCVector<CStr> SubscribePermissions{"TunnelManager/*"};
		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		co_return {};
	}

	TCFuture<void> CTunnelManagerServer::fp_Publish()
	{
		TCActorResultVector<void> PublishResults;
		//mp_ProtocolInterface.f_Publish<CCloudManager>(mp_AppState.m_DistributionManager, this) > PublishResults.f_AddResult();

		co_await PublishResults.f_GetUnwrappedResults();

		co_return {};
	}
}
