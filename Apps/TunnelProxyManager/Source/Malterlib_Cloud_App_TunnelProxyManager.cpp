// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
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

	TCFuture<void> CTunnelProxyManagerApp::fp_ReloadConfig(TCActorFunctor<TCFuture<void> (CStr _Message)> _fLog)
	{
		CLogError LogError("TunnelProxyManager");

		TCFutureVector<void> RemoveResults;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read Publish config");

			TCFutureMap<CStr, CActorSubscription> PublishResults;
			TCMap<CStr, CStr> NewHosts;

			if (auto const *pPublish = mp_State.m_ConfigDatabase.m_Data.f_GetMember("Publish"))
			{
				for (auto &PublishEntry : pPublish->f_Object())
				{
					auto &TunnelName = PublishEntry.f_Name();
					auto &Host = PublishEntry.f_Value()["Host"].f_String();

					NewHosts[TunnelName] = Host;

					if (auto pOldPublication = mp_Publications.f_FindEqual(TunnelName))
					{
						if (Host == pOldPublication->m_Host)
							continue;

						if (_fLog)
							co_await _fLog("Changing published tunnel '{}': {} -> {}"_f << TunnelName << pOldPublication->m_Host << Host);

						if (pOldPublication->m_Subscription)
							co_await fg_Exchange(pOldPublication->m_Subscription, nullptr)->f_Destroy();

					}
					else if (_fLog)
						co_await _fLog("Publishing new tunnel '{}': {}"_f << TunnelName << Host);

					mp_TunnelsServer(&CNetworkTunnelsServer::f_PublishNetworkTunnel, TunnelName, Host, 0, CEJsonSorted()) > PublishResults[TunnelName];
				}
			}

			auto NewPublish = co_await fg_AllDone(PublishResults);

			for (auto &PublishEntry : NewPublish.f_Entries())
			{
				auto &TunnelName = PublishEntry.f_Key();
				auto &Publication = mp_Publications[TunnelName];

				Publication.m_Subscription = fg_Move(fg_Move(PublishEntry.f_Value()));
				Publication.m_Host = NewHosts[TunnelName];
			}

			TCSet<CStr> ToRemove;
			for (auto &PublicationEntry : mp_Publications.f_Entries())
			{
				auto &TunnelName = PublicationEntry.f_Key();
				auto &Publication = PublicationEntry.f_Value();

				if (NewHosts.f_FindEqual(TunnelName))
					continue;

				if (Publication.m_Subscription)
					fg_Exchange(Publication.m_Subscription, nullptr)->f_Destroy() > RemoveResults;

				if (_fLog)
					co_await _fLog("Removing published tunnel '{}'"_f << TunnelName);

				ToRemove[TunnelName];
			}

			for (auto &Remove : ToRemove)
				mp_Publications.f_Remove(Remove);
		}

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Failed to read Subscribe config");

			TCFutureMap<CStr, CNetworkTunnelsClient::CTunnel> OpenTunnelResults;

			TCMap<CStr, CStr> NewListenHosts;

			if (auto const *pSubscribe = mp_State.m_ConfigDatabase.m_Data.f_GetMember("Subscribe"))
			{
				for (auto &SubscribeEntry : pSubscribe->f_Object())
				{
					auto &TunnelName = SubscribeEntry.f_Name();
					auto &ListenHost = SubscribeEntry.f_Value()["Listen"].f_String();

					NewListenHosts[TunnelName] = ListenHost;

					if (auto pOldSubscription = mp_Subscriptions.f_FindEqual(TunnelName))
					{
						if (ListenHost == pOldSubscription->m_ListenHost)
							continue;

						if (_fLog)
							co_await _fLog("Changing tunnel subscription '{}' listen: {} -> {}"_f << TunnelName << pOldSubscription->m_ListenHost << ListenHost);

						if (pOldSubscription->m_Tunnel.m_Subscription)
							fg_Exchange(pOldSubscription->m_Tunnel.m_Subscription, nullptr)->f_Destroy() > RemoveResults;
					}
					else if (_fLog)
						co_await _fLog("Opening new tunnel subscription '{}': {}"_f << TunnelName << ListenHost);

					CNetworkTunnelsClient::COpenTunnel OpenTunnel
						{
							.m_TunnelName = TunnelName
							, .m_fOnConnection = g_ActorFunctor / [this, TunnelName](CNetworkTunnelsClient::CCallbackInfo _CallbackInfo) -> TCFuture<void>
							{
								auto Auditor = mp_State.f_Auditor();
								Auditor.f_Info
									(
										"<{}> ({}) {{{}} {} New connection"_f
										<< _CallbackInfo.m_RemoteHostID
										<< _CallbackInfo.m_ConnectionID
										<< TunnelName
										<< _CallbackInfo.m_Address
									)
								;
								co_return {};
							}
							, .m_fOnClose = g_ActorFunctor / [this, TunnelName](CNetworkTunnelsClient::CCallbackInfo _CallbackInfo, NStr::CStr _Message) -> TCFuture<void>
							{
								auto Auditor = mp_State.f_Auditor();
								Auditor.f_Info
									(
										"<{}> ({}) {{{}} {} Connection closed: {}"_f
										<< _CallbackInfo.m_RemoteHostID
										<< _CallbackInfo.m_ConnectionID
										<< TunnelName
										<< _CallbackInfo.m_Address
										<< _Message
									)
								;
								co_return {};
							}
							, .m_fOnError = g_ActorFunctor / [this, TunnelName](CNetworkTunnelsClient::CCallbackInfo _CallbackInfo, NStr::CStr _Error) -> TCFuture<void>
							{
								auto Auditor = mp_State.f_Auditor();
								Auditor.f_Error
									(
										"<{}> ({}) {{{}} {} Connection error: {}"_f
										<< _CallbackInfo.m_RemoteHostID
										<< _CallbackInfo.m_ConnectionID
										<< TunnelName
										<< _CallbackInfo.m_Address
										<< _Error
									)
								;
								co_return {};
							}
							, .m_ListenHost = ListenHost
							, .m_bWaitForTunnel = true
						}
					;
					mp_TunnelsClient(&CNetworkTunnelsClient::f_OpenTunnel, fg_Move(OpenTunnel)) > OpenTunnelResults[TunnelName];
				}
			}

			auto NewTunnels = co_await fg_AllDone(OpenTunnelResults);

			for (auto &TunnelEntry : NewTunnels.f_Entries())
			{
				auto &TunnelName = TunnelEntry.f_Key();
				auto &Subscription = mp_Subscriptions[TunnelName];

				Subscription.m_Tunnel = fg_Move(TunnelEntry.f_Value());
				Subscription.m_ListenHost = NewListenHosts[TunnelName];
			}

			TCSet<CStr> ToRemove;
			for (auto &SubscriptionEntry : mp_Subscriptions.f_Entries())
			{
				auto &TunnelName = SubscriptionEntry.f_Key();
				auto &Subscription = SubscriptionEntry.f_Value();

				if (NewListenHosts.f_FindEqual(TunnelName))
					continue;

				if (Subscription.m_Tunnel.m_Subscription)
					fg_Exchange(Subscription.m_Tunnel.m_Subscription, nullptr)->f_Destroy() > RemoveResults;

				if (_fLog)
					co_await _fLog("Removing tunnel subscription '{}'"_f << TunnelName);

				ToRemove[TunnelName];
			}

			for (auto &Remove : ToRemove)
				mp_Subscriptions.f_Remove(Remove);
		}

		co_await fg_AllDone(RemoveResults).f_Wrap() > LogError.f_Warning("Failed to remove old subscriptions");

		co_return {};
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_StartApp(NEncoding::CEJsonSorted const _Params)
	{
		mp_TunnelsServer = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager, mp_State.f_AuditorFactory(), "TunnelProxyManager", "TunnelProxyManager");
		co_await mp_TunnelsServer(&CNetworkTunnelsServer::f_Start);

		mp_TunnelsClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);
		co_await mp_TunnelsClient(&CNetworkTunnelsClient::f_Start);

		co_await fp_ReloadConfig({});

		co_return {};
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_StopApp()
	{
		co_return {};
	}

	TCFuture<void> CTunnelProxyManagerApp::fp_Destroy()
	{
		TCFutureVector<void> Destroys;

		CLogError LogError("TunnelProxyManager");

		for (auto &Publication : mp_Publications)
			fg_Exchange(Publication.m_Subscription, nullptr)->f_Destroy() > Destroys;

		mp_Publications.f_Clear();

		for (auto &Subscription : mp_Subscriptions)
		{
			if (Subscription.m_Tunnel.m_Subscription)
				fg_Exchange(Subscription.m_Tunnel.m_Subscription, nullptr)->f_Destroy() > Destroys;
		}

		mp_Subscriptions.f_Clear();

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy tunnel subscriptions");

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
