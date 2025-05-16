// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/LogError>
#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>

#include "Malterlib_Cloud_App_DebugManagerClient.h"

namespace NMib::NCloud::NDebugManagerClient
{
	CDebugManagerClientApp::CDebugManagerClientApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"DebugManagerClient"})
	{
	}
	
	CDebugManagerClientApp::~CDebugManagerClientApp()
	{
	}

	TCFuture<void> CDebugManagerClientApp::fp_SubscribeToManagers()
	{
		if (!mp_DebugManagers.f_IsEmpty())
			co_return {};

		mp_DebugManagers = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CDebugManager>();

		co_return {};
	}

	TCFuture<void> CDebugManagerClientApp::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		co_await (fp_SubscribeToManagers() % "Falied to subscribe to debug managers");

		co_await (fp_StartWebServer() % "Failed to start web server");

		co_return {};
	}

	TCFuture<void> CDebugManagerClientApp::fp_StopApp()
	{	
		co_return {};
	}

	TCFuture<void> CDebugManagerClientApp::fp_Destroy()
	{
		TCFutureVector<void> Destroys;

		CLogError LogError("DebugManagerClient");

		mp_DebugManagers.f_Destroy() > Destroys;

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy dependencies");

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_DebugManagerClient()
	{
		return fg_Construct<NDebugManagerClient::CDebugManagerClientApp>();
	}
}
