// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include <Mib/Network/AsyncSocket>
#include <Mib/Cryptography/RandomID>

#include "Malterlib_Cloud_NetworkTunnelsServer.h"

namespace NMib::NCloud
{
	using namespace NStr;
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NNetwork;
	using namespace NStorage;
	using namespace NEncoding;
	using namespace NFunction;
	using namespace NCryptography;

	struct CNetworkTunnelsServer::CInternal : public CActorInternal
	{
		CInternal
			(
				CNetworkTunnelsServer *_pThis
				, TCActor<CActorDistributionManager> const &_DistributionManager
				, TCActor<CDistributedActorTrustManager> const &_TrustManager
				, TCFunctionMovable<CDistributedAppAuditor (CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> &&_AuditorFactory
				, CStr const &_LogCategory
				, CStr const &_PermissionPrefix
			)
			: m_pThis(_pThis)
			, m_DistributionManager(_DistributionManager)
			, m_TrustManager(_TrustManager)
			, m_AuditorFactory(fg_Move(_AuditorFactory))
			, m_LogCategory(_LogCategory)
			, m_PermissionPrefix(_PermissionPrefix)
		{
		}

		struct CNetworkTunnel
		{
			CStr m_Host;
			uint16 m_Port = 0;
			CEJSONSorted m_MetaData;
		};

		struct CConnection
		{
			TCActorInterface<CAsyncSocketActor> m_Socket;
			ICNetworkTunnels::FSendBytes m_fSendData;
		};

		struct CNetworkTunnelImplementation : public ICNetworkTunnels
		{
			TCFuture<TCMap<CNetworkTunnelName, CNetworkTunnel>> f_EnumerateTunnels() override
			{
				auto &Internal = *m_pThis->mp_pInternal;

				auto AppAuditor = Internal.m_AuditorFactory(fg_GetCallingHostInfo(), {});

				TCSet<CNetworkTunnelName> Tunnels;

				TCMap<CStr, TCVector<CPermissionQuery>> Permissions;
				Permissions["//ALL//"] = {{"{}/ConnectAll"_f << Internal.m_PermissionPrefix}};
				for (auto &Tunnel : Internal.m_NetworkTunnels)
				{
					auto &TunnelName = Internal.m_NetworkTunnels.fs_GetKey(Tunnel);
					Permissions[TunnelName] =
						{
							CPermissionQuery{"{}/Connect/{}"_f << Internal.m_PermissionPrefix << TunnelName}.f_Description("Connect to {} tunnel"_f << TunnelName)
						}
					;
				}

				auto HasPermissions = co_await (Internal.m_Permissions.f_HasPermissions("Enum tunnels", Permissions) % AppAuditor);

				TCMap<CNetworkTunnelName, CNetworkTunnel> ReturnTunnels;
				TCSet<CNetworkTunnelName> TunnelNames;
				bool bAccessAll = HasPermissions["//ALL//"];

				for (auto &Tunnel : Internal.m_NetworkTunnels)
				{
					auto &TunnelName = Internal.m_NetworkTunnels.fs_GetKey(Tunnel);
					if (!bAccessAll && !HasPermissions[TunnelName])
						continue;
					ReturnTunnels[TunnelName].m_MetaData = Tunnel.m_MetaData;
					TunnelNames[TunnelName];
				}

				AppAuditor.f_Info("Enumerated tunnels: {vs}"_f << TunnelNames);

				co_return ReturnTunnels;
			}

			TCFuture<TCActorSubscriptionWithID<>> f_SubscribeTunnels(CSubscribeTunnels &&_Subscribe) override
			{
				auto &Internal = *m_pThis->mp_pInternal;

				auto CallingHostInfo = fg_GetCallingHostInfo();

				auto AppAuditor = Internal.m_AuditorFactory(CallingHostInfo, {});

				auto SubscriptionID = fg_RandomID(Internal.m_ChangeSubscriptions);

				auto &Subscription = Internal.m_ChangeSubscriptions[SubscriptionID];
				Subscription.m_Subscription = fg_Move(_Subscribe);
				Subscription.m_CallingHostInfo = CallingHostInfo;
				
				auto SubscriptionHandle = g_ActorSubscription / [pThis = m_pThis, SubscriptionID]() -> TCFuture<void>
					{
						auto &Internal = *pThis->mp_pInternal;

						auto pChangeSubscription = Internal.m_ChangeSubscriptions.f_FindEqual(SubscriptionID);

						if (!pChangeSubscription)
							co_return {};

						auto fOnTunnelChange = fg_Move(pChangeSubscription->m_Subscription.m_fOnTunnelChange);
						Internal.m_ChangeSubscriptions.f_Remove(pChangeSubscription);

						co_await fg_Move(fOnTunnelChange).f_Destroy();

						co_return {};
					}
				;

				TCMap<CStr, TCVector<CPermissionQuery>> Permissions;
				Permissions["//ALL//"] = {{"{}/ConnectAll"_f << Internal.m_PermissionPrefix}};
				for (auto &Tunnel : Internal.m_NetworkTunnels)
				{
					auto &TunnelName = Internal.m_NetworkTunnels.fs_GetKey(Tunnel);
					Permissions[TunnelName] =
						{
							CPermissionQuery{"{}/Connect/{}"_f << Internal.m_PermissionPrefix << TunnelName}.f_Description("Connect to {} tunnel"_f << TunnelName)
						}
					;
				}

				auto HasPermissions = co_await (Internal.m_Permissions.f_HasPermissions("Enum tunnels", Permissions) % AppAuditor);
				bool bAccessAll = HasPermissions["//ALL//"];

				ICNetworkTunnels::CTunnelChange_Initial Change;
				for (auto &Tunnel : Internal.m_NetworkTunnels)
				{
					auto &TunnelName = Internal.m_NetworkTunnels.fs_GetKey(Tunnel);

					if (!bAccessAll && !HasPermissions[TunnelName])
						continue;
					
					Change.m_Tunnels[TunnelName].m_MetaData = Tunnel.m_MetaData;
				}

				co_await (Subscription.m_Subscription.m_fOnTunnelChange(fg_Move(Change)) % AppAuditor);

				AppAuditor.f_Info("Subscribed to tunnels");

				co_return fg_Move(SubscriptionHandle);
			}

			TCFuture<FSendBytes> f_OpenConnection(COpenConnection &&_OpenConnection) override
			{
				auto &Internal = *m_pThis->mp_pInternal;

				auto AppAuditor = Internal.m_AuditorFactory(fg_GetCallingHostInfo(), {});

				TCVector<CStr> Permissions = {"{}/ConnectAll"_f << Internal.m_PermissionPrefix, "{}/{}/Connect"_f << Internal.m_PermissionPrefix << _OpenConnection.m_Name};

				bool bHasPermisions = co_await (Internal.m_Permissions.f_HasPermission("Open connection", Permissions) % AppAuditor);

				if (!bHasPermisions)
					co_return AppAuditor.f_AccessDenied("(Open connection)", Permissions);

				auto pTunnel = Internal.m_NetworkTunnels.f_FindEqual(_OpenConnection.m_Name);
				if (!pTunnel)
					co_return DMibErrorInstance("No such network tunnel");

				auto NewConnection = co_await
					(
						Internal.m_SocketClient(&CAsyncSocketClientActor::f_Connect, pTunnel->m_Host, "", ENetAddressType_None, pTunnel->m_Port, nullptr) % AppAuditor
					)
				;

				mint ConnectionID = ++Internal.m_ConnectionID;

				auto &Connection = Internal.m_Connections[ConnectionID];

				auto fCleanupConnection = [pThis = m_pThis, ConnectionID, RemoteConnectionID = _OpenConnection.m_ConnectionID, Name = _OpenConnection.m_Name, AppAuditor]
					(CStr const &_Description) -> TCFuture<void>
					{
						co_await ECoroutineFlag_AllowReferences;

						auto &Internal = *pThis->mp_pInternal;
						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (pConnection)
						{
							AppAuditor.f_Info("({} - {}) {{{}} Tunnel connection closed: {}"_f << RemoteConnectionID << ConnectionID << Name << _Description);

							auto Connection = fg_Move(*pConnection);
							Internal.m_Connections.f_Remove(pConnection);

							if (Connection.m_Socket)
								co_await Connection.m_Socket.f_Destroy();
							co_await fg_Move(Connection.m_fSendData).f_Destroy();
						}
						co_return {};
					}
				;

				auto Cleanup = g_OnScopeExitActor / [pThis = m_pThis, fCleanupConnection]() mutable
					{
						(pThis->self.f_Invoke(fg_Move(fCleanupConnection), "Cleanup")) > fg_LogWarning("NetworkTunnelsServer", "Failed to cleanup connection");
					}
				;

				Connection.m_fSendData = fg_Move(_OpenConnection.m_fOnReceive);

				CAsyncSocketCallbacks SocketCallbacks;
				SocketCallbacks.m_fOnClose = g_ActorFunctor / [fCleanupConnection, AllowDestroy = g_AllowWrongThreadDestroy]
					(EAsyncSocketStatus _Reason, CStr &&_Message, EAsyncSocketCloseOrigin _Origin) -> TCFuture<void>
					{
						co_await fCleanupConnection(_Message);
						co_return {};
					}
				;

				SocketCallbacks.m_fOnReceiveData = g_ActorFunctor / [pThis = m_pThis, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy]
					(TCSharedPointer<CSecureByteVector> &&_pMessage) -> TCFuture<void>
					{
						auto &Internal = *pThis->mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (!pConnection)
							co_return DMibErrorInstance("Socket no longer exists");

						co_await pConnection->m_fSendData(fg_Move(*_pMessage));
						co_return {};
					}
				;

				Connection.m_Socket = co_await (NewConnection.f_Accept(fg_Move(SocketCallbacks)) % AppAuditor);

				Cleanup->f_Clear();

				AppAuditor.f_Info("({} - {}) {{{}} Tunnel connection opened"_f << _OpenConnection.m_ConnectionID << ConnectionID << _OpenConnection.m_Name);

				co_return g_ActorFunctor
					(
						g_ActorSubscription / [fCleanupConnection]() -> TCFuture<void>
						{
							co_await fCleanupConnection("Remote closed tunnel connection");
							co_return {};
						}
					)
					/ [pThis = m_pThis, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy, AppAuditor](CSecureByteVector &&_Data) -> TCFuture<void>
					{
						auto &Internal = *pThis->mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (!pConnection)
							co_return DMibErrorInstance("Socket no longer exists");
						co_await pConnection->m_Socket(&CAsyncSocketActor::f_SendData, fg_Construct(fg_Move(_Data)), 0);
						co_return {};
					}
				;
			}

			DMibDelegatedActorImplementation(CNetworkTunnelsServer);
		};

		struct CTunnelsChangeSubscription
		{
			ICNetworkTunnels::CSubscribeTunnels m_Subscription;
			CCallingHostInfo m_CallingHostInfo;
		};

		TCFuture<void> f_SetupPermissions();
		TCFuture<void> f_SendChange(ICNetworkTunnels::CTunnelChange _Change, CStr _Name);

		CNetworkTunnelsServer *m_pThis;
		TCActor<CActorDistributionManager> m_DistributionManager;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		CStr m_LogCategory;
		CStr m_PermissionPrefix;

		TCFunctionMovable<CDistributedAppAuditor (CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> m_AuditorFactory;
		TCDistributedActorInstance<CNetworkTunnelImplementation> m_NetworkTunnelInstance;
		CTrustedPermissionSubscription m_Permissions;

		TCMap<CStr, CNetworkTunnel> m_NetworkTunnels;
		TCActor<CAsyncSocketClientActor> m_SocketClient = fg_Construct();
		TCMap<mint, CConnection> m_Connections;
		TCMap<CStr, CTunnelsChangeSubscription> m_ChangeSubscriptions;

		mint m_ConnectionID = 0;
	};

	CNetworkTunnelsServer::CNetworkTunnelsServer
		(
			TCActor<CActorDistributionManager> const &_DistributionManager
			, TCActor<CDistributedActorTrustManager> const &_TrustManager
			, TCFunctionMovable<CDistributedAppAuditor (CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> &&_AuditorFactory
			, CStr const &_LogCategory
			, CStr const &_PermissionPrefix
		)
		: mp_pInternal(fg_Construct(this, _DistributionManager, _TrustManager, fg_Move(_AuditorFactory), _LogCategory, _PermissionPrefix))
	{
	}

	CNetworkTunnelsServer::~CNetworkTunnelsServer() = default;

	TCFuture<void> CNetworkTunnelsServer::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;
		CLogError LogError("NetworkTunnelsServer");

		TCActorResultVector<void> Destroys;
		for (auto &Connection : Internal.m_Connections)
		{
			if (Connection.m_Socket)
				fg_Move(Connection.m_Socket).f_Destroy() > Destroys.f_AddResult();
			fg_Move(Connection.m_fSendData).f_Destroy() > Destroys.f_AddResult();
		}
		Internal.m_Connections.f_Clear();

		co_await Destroys.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy connections");
		co_await Internal.m_NetworkTunnelInstance.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to network tunnel instance");
		co_return {};
	}

	TCFuture<void> CNetworkTunnelsServer::CInternal::f_SetupPermissions()
	{
		TCSet<CStr> Permissions{"{}/ConnectAll"_f << m_PermissionPrefix};
		co_await m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));

		TCVector<CStr> SubscribePermissions{"{}/*"_f << m_PermissionPrefix};
		m_Permissions = co_await m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, fg_Move(SubscribePermissions), fg_ThisActor(m_pThis));

		co_return {};
	}

	TCFuture<void> CNetworkTunnelsServer::f_Start()
	{
		auto &Internal = *mp_pInternal;

		co_await Internal.m_SocketClient(&CAsyncSocketClientActor::f_SetDefaultTimeout, 0.0);
		co_await Internal.f_SetupPermissions();
		co_await Internal.m_NetworkTunnelInstance.f_Publish<ICNetworkTunnels>(Internal.m_DistributionManager, this);

		co_return {};
	}

	TCFuture<void> CNetworkTunnelsServer::CInternal::f_SendChange(ICNetworkTunnels::CTunnelChange _Change, CStr _Name)
	{
		TCActorResultMap<CStr, bool> PermissionResults;
		for (auto &SubscriptionEntry : m_ChangeSubscriptions.f_Entries())
		{
			auto &SubscriptionID = SubscriptionEntry.f_Key();
			
			m_Permissions.f_HasPermission
				(
					"Open connection"
					, TCVector<CStr>{"{}/ConnectAll"_f << m_PermissionPrefix, "{}/{}/Connect"_f << m_PermissionPrefix << _Name}
					, SubscriptionEntry.f_Value().m_CallingHostInfo
				)
				> PermissionResults.f_AddResult(SubscriptionID)
			;
		}

		auto Permissions = co_await PermissionResults.f_GetUnwrappedResults();

		TCActorResultVector<void> SubscriptionResults;
		for (auto &PermissionEntry : Permissions.f_Entries())
		{
			if (!PermissionEntry.f_Value())
				continue;

			auto &SubscriptionID = PermissionEntry.f_Key();
			auto *pChangeSubscription = m_ChangeSubscriptions.f_FindEqual(SubscriptionID);
			if (!pChangeSubscription)
				continue;

			pChangeSubscription->m_Subscription.m_fOnTunnelChange(fg_TempCopy(_Change)) > SubscriptionResults.f_AddResult();
		}

		co_await SubscriptionResults.f_GetUnwrappedResults();

		co_return {};
	}

	TCFuture<CActorSubscription> CNetworkTunnelsServer::f_PublishNetworkTunnel
		(
			ICNetworkTunnels::CNetworkTunnelName const &_Name
			, CStr const &_Host
			, uint16 _Port
			, CEJSONSorted &&_MetaData
		)
	{
		auto &Internal = *mp_pInternal;

		auto MetaData = _MetaData;

		auto TunnelMap = Internal.m_NetworkTunnels(_Name, CInternal::CNetworkTunnel{_Host, _Port, fg_Move(_MetaData)});
		if (!TunnelMap.f_WasCreated())
			co_return DMibErrorInstance("A tunnel with the same name is already published");

		auto Permissions = TCSet<CStr>{fg_Format("{}/Connect/{}", Internal.m_PermissionPrefix, _Name)};
		co_await Internal.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));

		if (!Internal.m_ChangeSubscriptions.f_IsEmpty())
		{
			ICNetworkTunnels::CTunnelChange_Add Change;
			Change.m_TunnelName = _Name;
			Change.m_Tunnel.m_MetaData = fg_Move(MetaData);

			co_await Internal.f_SendChange(fg_Move(Change), _Name).f_Wrap() > fg_LogError("NetworkTunnelsServer", "Failed to send tunnel changes to subscribers");
		}

		co_return
			(
				g_ActorSubscription / [this, Permissions, _Name]() mutable -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;
					co_await Internal.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, fg_Move(Permissions)).f_Wrap()
						> fg_LogWarning("NetworkTunnelsServer", "Failed to desroy network tunnel publication");
					;
					Internal.m_NetworkTunnels.f_Remove(_Name);

					if (!Internal.m_ChangeSubscriptions.f_IsEmpty())
					{
						ICNetworkTunnels::CTunnelChange_Remove Change;
						Change.m_TunnelName = _Name;

						co_await Internal.f_SendChange(fg_Move(Change), _Name).f_Wrap() > fg_LogError("NetworkTunnelsServer", "Failed to send tunnel changes to subscribers");
					}

					co_return {};
				}
			)
		;
	}
}
