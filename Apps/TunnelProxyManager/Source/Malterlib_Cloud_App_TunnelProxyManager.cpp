// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_TunnelProxyManager.h"

namespace NMib::NCloud::NTunnelProxyManager
{
	CTunnelProxyManagerApp::CTunnelProxyManagerApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"TunnelProxyManager"})
	{
	}
	
	CTunnelProxyManagerApp::~CTunnelProxyManagerApp()
	{
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_StartApp(NEncoding::CEJSONSorted const &_Params)
	{
		mp_TunnelsServer = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager, mp_State.f_AuditorFactory(), "TunnelProxyManager", "TunnelProxyManager");
		co_await mp_TunnelsServer(&CNetworkTunnelsServer::f_Start);

		if (auto const *pPublish = mp_State.m_ConfigDatabase.m_Data.f_GetMember("Publish"))
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read Publish config");

			TCActorResultVector<CActorSubscription> PublishResults;
			for (auto &PublishEntry : pPublish->f_Object())
			{
				mp_TunnelsServer(&CNetworkTunnelsServer::f_PublishNetworkTunnel, PublishEntry.f_Name(), PublishEntry.f_Value()["Host"].f_String(), 0, CEJSONSorted()) 
					> PublishResults.f_AddResult()
				;
			}

			mp_TunnelPublicationSubscriptions = co_await PublishResults.f_GetUnwrappedResults();
		}

		mp_TunnelsClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);
		co_await mp_TunnelsClient(&CNetworkTunnelsClient::f_Start);

		if (auto const *pSubscribe = mp_State.m_ConfigDatabase.m_Data.f_GetMember("Subscribe"))
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read Subscribe config");

			TCActorResultVector<CNetworkTunnelsClient::CTunnel> OpenTunnelResults;

			for (auto &SubscribeEntry : pSubscribe->f_Object())
			{
				CNetworkTunnelsClient::COpenTunnel OpenTunnel
					{
						.m_TunnelName = SubscribeEntry.f_Name()
						, .m_fOnConnection = g_ActorFunctor / [this](NMib::NNetwork::CNetAddress const &_Address) -> TCFuture<void>
						{
							auto Auditor = mp_State.f_Auditor();
							Auditor.f_Info("New connection from address: {}"_f << _Address);
							co_return {};
						}
						, .m_fOnClose = g_ActorFunctor / [this](NMib::NNetwork::CNetAddress const &_Address, NStr::CStr const &_Message) -> TCFuture<void>
						{
							auto Auditor = mp_State.f_Auditor();
							Auditor.f_Info("Connection from address '{}' closed: {}"_f << _Address << _Message);
							co_return {};
						}
						, .m_fOnError = g_ActorFunctor / [this](NMib::NNetwork::CNetAddress const &_Address, NStr::CStr const &_Error) -> TCFuture<void>
						{
							auto Auditor = mp_State.f_Auditor();
							Auditor.f_Error("Connection error from address '{}': {}"_f << _Address << _Error);
							co_return {};
						}
						, .m_ListenHost = SubscribeEntry.f_Value()["Listen"].f_String()
						, .m_bWaitForTunnel = true
					}
				;
				mp_TunnelsClient(&CNetworkTunnelsClient::f_OpenTunnel, fg_Move(OpenTunnel)) > OpenTunnelResults.f_AddResult();
			}

			mp_Tunnels = co_await OpenTunnelResults.f_GetUnwrappedResults();
		}

		co_return {};
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_StopApp()
	{	
		co_return {};
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_Destroy()
	{
		TCActorResultVector<void> Destroys;

		CLogError LogError("TunnelProxyManager");

		for (auto &Subscription : mp_TunnelPublicationSubscriptions)
			fg_Exchange(Subscription, nullptr)->f_Destroy() > Destroys.f_AddResult();

		mp_TunnelPublicationSubscriptions.f_Clear();

		for (auto &Tunnel : mp_Tunnels)
		{
			if (Tunnel.m_Subscription)
				fg_Exchange(Tunnel.m_Subscription, nullptr)->f_Destroy() > Destroys.f_AddResult();
		}

		mp_Tunnels.f_Clear();

		co_await Destroys.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy tunnel subscriptions");

		if (mp_TunnelsClient)
			co_await fg_Move(mp_TunnelsClient).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy tunnels client");

		if (mp_TunnelsServer)
			co_await fg_Move(mp_TunnelsServer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy tunnels server");

		co_await CDistributedAppActor::fp_Destroy();

		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_TunnelProxyManager()
	{
		return fg_Construct<NTunnelProxyManager::CTunnelProxyManagerApp>();
	}
}
