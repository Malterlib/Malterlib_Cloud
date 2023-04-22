// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include <Mib/Network/AsyncSocket>

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
			CEJSON m_MetaData;
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

			TCFuture<FSendBytes> f_OpenConnection(CNetworkTunnelName const &_Name, FSendBytes &&_fOnReceive) override
			{
				auto &Internal = *m_pThis->mp_pInternal;

				auto AppAuditor = Internal.m_AuditorFactory(fg_GetCallingHostInfo(), {});

				TCVector<CStr> Permissions = {"{}/ConnectAll"_f << Internal.m_PermissionPrefix, "{}/{}/Connect"_f << Internal.m_PermissionPrefix << _Name};

				bool bHasPermisions = co_await (Internal.m_Permissions.f_HasPermission("Open connection", Permissions) % AppAuditor);

				if (!bHasPermisions)
					co_return AppAuditor.f_AccessDenied("(Download backup)", Permissions);

				auto pTunnel = Internal.m_NetworkTunnels.f_FindEqual(_Name);
				if (!pTunnel)
					co_return DMibErrorInstance("No such network tunnel");

				auto NewConnection = co_await
					(
						Internal.m_SocketClient(&CAsyncSocketClientActor::f_Connect, pTunnel->m_Host, "", ENetAddressType_None, pTunnel->m_Port, nullptr) % AppAuditor
					)
				;

				mint ConnectionID = ++Internal.m_ConnectionID;

				auto &Connection = Internal.m_Connections[ConnectionID];

				auto fCleanupConnection = [pThis = m_pThis, ConnectionID]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_AllowReferences;

						auto &Internal = *pThis->mp_pInternal;
						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (pConnection)
						{
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
						(pThis->self / fg_Move(fCleanupConnection)) > fg_LogWarning("NetworkTunnelsServer", "Failed to cleanup connection");
					}
				;

				Connection.m_fSendData = fg_Move(_fOnReceive);

				CAsyncSocketCallbacks SocketCallbacks;
				SocketCallbacks.m_fOnClose = g_ActorFunctor / [fCleanupConnection, AllowDestroy = g_AllowWrongThreadDestroy]
					(EAsyncSocketStatus _Reason, CStr const &_Message, EAsyncSocketCloseOrigin _Origin) -> TCFuture<void>
					{
						co_await fCleanupConnection();
						co_return {};
					}
				;

				SocketCallbacks.m_fOnReceiveData = g_ActorFunctor / [pThis = m_pThis, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy]
					(TCSharedPointer<CSecureByteVector> const &_pMessage) -> TCFuture<void>
					{
						auto &Internal = *pThis->mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (!pConnection)
							co_return DMibErrorInstance("Socket no longer exists");

						co_await pConnection->m_fSendData(*_pMessage);
						co_return {};
					}
				;

				Connection.m_Socket = co_await (NewConnection.f_Accept(fg_Move(SocketCallbacks)) % AppAuditor);

				Cleanup->f_Clear();

				AppAuditor.f_Info("Opened tunnel connection: {}"_f << _Name);

				co_return g_ActorFunctor
					(
						g_ActorSubscription / fCleanupConnection
					)
					/ [pThis = m_pThis, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy](CSecureByteVector &&_Data) -> TCFuture<void>
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

		TCFuture<void> f_SetupPermissions();

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
		NConcurrency::CLogError LogError("NetworkTunnelsServer");

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

		co_await Internal.f_SetupPermissions();
		co_await Internal.m_NetworkTunnelInstance.f_Publish<ICNetworkTunnels>(Internal.m_DistributionManager, this);
		co_return {};
	}

	TCFuture<CActorSubscription> CNetworkTunnelsServer::f_PublishNetworkTunnel
		(
			ICNetworkTunnels::CNetworkTunnelName const &_Name
			, CStr const &_Host
			, uint16 _Port
			, CEJSON &&_MetaData
		)
	{
		auto &Internal = *mp_pInternal;

		auto TunnelMap = Internal.m_NetworkTunnels(_Name, CInternal::CNetworkTunnel{_Host, _Port, fg_Move(_MetaData)});
		if (!TunnelMap.f_WasCreated())
			co_return DMibErrorInstance("A tunnel with the same name is already published");

		auto Permissions = TCSet<CStr>{fg_Format("{}/Connect/{}", Internal.m_PermissionPrefix, _Name)};
		co_await Internal.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));

		co_return
			(
				g_ActorSubscription / [this, Permissions, _Name]() mutable -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;
					co_await Internal.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, fg_Move(Permissions)).f_Wrap()
						> fg_LogWarning("NetworkTunnelsServer", "Failed to desroy network tunnel publication");
					;
					Internal.m_NetworkTunnels.f_Remove(_Name);
					co_return {};
				}
			)
		;
	}
}
