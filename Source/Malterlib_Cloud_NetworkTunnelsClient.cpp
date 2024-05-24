// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Network/AsyncSocket>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_NetworkTunnelsClient.h"

namespace NMib::NCloud
{
	using namespace NStr;
	using namespace NConcurrency;
	using namespace NContainer;
	using namespace NNetwork;
	using namespace NStorage;

	struct CNetworkTunnelsClient::CInternal : public CActorInternal
	{
		CInternal
			(
				CNetworkTunnelsClient *_pThis
				, TCActor<CActorDistributionManager> const &_DistributionManager
				, TCActor<CDistributedActorTrustManager> const &_TrustManager
			)
			: m_pThis(_pThis)
			, m_DistributionManager(_DistributionManager)
			, m_TrustManager(_TrustManager)
		{
		}

		struct CConnection
		{
			TCFuture<void> f_Destroy()
			{
				TCPromise<void> Promise;
				TCActorResultVector<void> Destroys;

				if (m_Socket)
					fg_Move(m_Socket).f_Destroy() > Destroys.f_AddResult();
				fg_Move(m_fSendData).f_Destroy() > Destroys.f_AddResult();

				Destroys.f_GetUnwrappedResults() > Promise.f_ReceiveAny();
				return Promise.f_MoveFuture();
			}

			TCActorInterface<CAsyncSocketActor> m_Socket;
			ICNetworkTunnels::FSendBytes m_fSendData;
		};

		struct CTunnel
		{
			TCFuture<void> f_Destroy()
			{
				TCPromise<void> Promise;
				TCActorResultVector<void> Destroys;

				if (m_ListenSubscription)
					m_ListenSubscription->f_Destroy() > Destroys.f_AddResult();

				fg_Move(m_fOnError).f_Destroy() > Destroys.f_AddResult();
				fg_Move(m_fOnClose).f_Destroy() > Destroys.f_AddResult();
				fg_Move(m_fOnConnection).f_Destroy() > Destroys.f_AddResult();

				if (m_TunnelActor)
					fg_Move(m_TunnelActor).f_Destroy() > Destroys.f_AddResult();

				Destroys.f_GetUnwrappedResults() > Promise.f_ReceiveAny();
				return Promise.f_MoveFuture();
			}

			TCFuture<void> f_OnConnection(CNetAddress const &_Address)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnConnection)
					co_return {};

				co_await m_fOnConnection(_Address).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on connection on tunnel");

				co_return {};
			}

			TCFuture<void> f_OnClose(CNetAddress const &_Address, CStr const &_Message)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnClose)
					co_return {};

				co_await m_fOnClose(_Address, _Message).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on close on tunnel");

				co_return {};
			}

			TCFuture<void> f_OnError(CNetAddress const &_Address, CStr const &_Error)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnError)
					co_return {};

				co_await m_fOnError(_Address, _Error).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on error on tunnel");

				co_return {};
			}

			ICNetworkTunnels::CNetworkTunnelName m_TunnelName;
			TCDistributedActor<ICNetworkTunnels> m_TunnelActor;
			CActorSubscription m_ListenSubscription;
			TCActorFunctor<TCFuture<void> (CNetAddress const &_Address)> m_fOnConnection;
			TCActorFunctor<TCFuture<void> (CNetAddress const &_Address, CStr const &_Message)> m_fOnClose;
			TCActorFunctor<TCFuture<void> (CNetAddress const &_Address, CStr const &_Error)> m_fOnError;
		};

		TCFuture<void> f_Subscribe();

		CNetworkTunnelsClient *m_pThis;
		TCActor<CActorDistributionManager> m_DistributionManager;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		TCTrustedActorSubscription<ICNetworkTunnels> m_NetworkTunnelSubscription;
		TCActor<CAsyncSocketServerActor> m_SocketServer = fg_Construct();
		TCMap<mint, CConnection> m_Connections;
		TCMap<mint, TCSharedPointer<CTunnel>> m_Tunnels;
		TCActor<NNetwork::CResolveActor> m_AddressResolver;

		mint m_ConnectionID = 0;
		mint m_TunnelID = 0;
	};

	CNetworkTunnelsClient::CNetworkTunnelsClient
		(
			TCActor<CActorDistributionManager> const &_DistributionManager
			, TCActor<CDistributedActorTrustManager> const &_TrustManager
		)
		: mp_pInternal(fg_Construct(this, _DistributionManager, _TrustManager))
	{
	}

	CNetworkTunnelsClient::~CNetworkTunnelsClient() = default;

	TCFuture<void> CNetworkTunnelsClient::f_Start()
	{
		auto &Internal = *mp_pInternal;
		co_await Internal.f_Subscribe();

		co_return {};
	}

	TCFuture<void> CNetworkTunnelsClient::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		NConcurrency::CLogError LogError("NetworkTunnelsClient");

		TCActorResultVector<void> Results;
		for (auto &Connection : Internal.m_Connections)
		{
			if (Connection.m_Socket)
				fg_Move(Connection.m_Socket).f_Destroy() > Results.f_AddResult();
			fg_Move(Connection.m_fSendData).f_Destroy() > Results.f_AddResult();
		}
		Internal.m_Connections.f_Clear();

		for (auto &Tunnel : Internal.m_Tunnels)
			Tunnel->f_Destroy() > Results.f_AddResult();
		Internal.m_Tunnels.f_Clear();

		if (Internal.m_AddressResolver)
			fg_Move(Internal.m_AddressResolver).f_Destroy() > Results.f_AddResult();

		co_await Results.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy tunnels client");

		co_await Internal.m_NetworkTunnelSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy tunnels client subscription");

		co_return {};
	}

	TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> CNetworkTunnelsClient::f_EnumTunnels()
	{
		auto &Internal = *mp_pInternal;

		TCActorResultMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>> TunnelResults;
		for (auto &Actor : Internal.m_NetworkTunnelSubscription.m_Actors)
			Actor.m_Actor.f_CallActor(&ICNetworkTunnels::f_EnumerateTunnels)() > TunnelResults.f_AddResult(Actor.m_TrustInfo.m_HostInfo.m_HostID);

		co_return co_await (co_await TunnelResults.f_GetResults() | g_Unwrap);
	}

	auto CNetworkTunnelsClient::f_OpenTunnel
		(
			CStr const &_HostID
			, ICNetworkTunnels::CNetworkTunnelName const &_TunnelName
			, TCActorFunctor<TCFuture<void> (CNetAddress const &_Address)> &&_fOnConnection
			, TCActorFunctor<TCFuture<void> (CNetAddress const &_Address, CStr const &_Message)> &&_fOnClose
			, TCActorFunctor<TCFuture<void> (CNetAddress const &_Address, CStr const &_Error)> &&_fOnError
			, CStr const &_ListenHost
		)
		-> TCFuture<CTunnel>
	{
		auto &Internal = *mp_pInternal;

		mint TunnelID = ++Internal.m_TunnelID;
		TCSharedPointer<CInternal::CTunnel> pTunnel = fg_Construct();

		Internal.m_Tunnels[TunnelID] = pTunnel;
		auto CleanupTunnel = g_OnScopeExitActor / [this, TunnelID]
			{
				auto &Internal = *mp_pInternal;
				auto pTunnel = Internal.m_Tunnels.f_FindEqual(TunnelID);
				if (pTunnel)
				{
					(*pTunnel)->f_Destroy() > fg_LogError("Network tunnel client", "Failed to destroy tunnel");
					Internal.m_Tunnels.f_Remove(TunnelID);
				}
			}
		;

		pTunnel->m_TunnelName = _TunnelName;
		pTunnel->m_fOnConnection = fg_Move(_fOnConnection);
		pTunnel->m_fOnClose = fg_Move(_fOnClose);
		pTunnel->m_fOnError = fg_Move(_fOnError);

		for (auto &Actor : Internal.m_NetworkTunnelSubscription.m_Actors)
		{
			if (Actor.m_TrustInfo.m_HostInfo.m_HostID == _HostID)
			{
				pTunnel->m_TunnelActor = Actor.m_Actor;
				break;
			}
		}
		if (!pTunnel->m_TunnelActor)
			co_return DMibErrorInstance("No tunnel subscription found for this host ID");

		CAsyncSocketServerCallbacks Callbacks;

		Callbacks.m_fNewConnection = g_ActorFunctor / [this, pTunnel, AllowDestroy = g_AllowWrongThreadDestroy](CAsyncSocketNewServerConnection &&_Connection) mutable -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;

				mint ConnectionID = ++Internal.m_ConnectionID;

				auto &Connection = Internal.m_Connections[ConnectionID];

				auto Cleanup = g_OnScopeExitActor / [this, ConnectionID]
					{
						auto &Internal = *mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (!pConnection)
							return;
						pConnection->f_Destroy() > fg_LogError("Network tunnel client", "Failed to destroy connection");
						Internal.m_Connections.f_Remove(pConnection);
					}
				;

				auto OpenConnectionResult = co_await pTunnel->m_TunnelActor.f_CallActor(&ICNetworkTunnels::f_OpenConnection)
					(
						pTunnel->m_TunnelName
						, g_ActorFunctor
						(
							g_ActorSubscription / [this, ConnectionID]() -> TCFuture<void>
							{
								auto &Internal = *mp_pInternal;
								auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
								if (!pConnection)
									co_return {};

								auto DestroyFuture = pConnection->f_Destroy();
								Internal.m_Connections.f_Remove(pConnection);
								co_await fg_Move(DestroyFuture).f_Wrap() > fg_LogError("Network tunnel client", "Failed to destroy connection");

								co_return {};
							}
						)
						/ [this, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy](CSecureByteVector &&_Data) -> TCFuture<void>
						{
							auto &Internal = *mp_pInternal;

							auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
							if (!pConnection || !pConnection->m_Socket)
								co_return DMibErrorInstance("Socket no longer exists");
							
							co_await pConnection->m_Socket(&CAsyncSocketActor::f_SendData, fg_Construct(fg_Move(_Data)), 0);
							co_return {};
						}
					)
					.f_Wrap()
				;

				if (!OpenConnectionResult)
				{
					co_await pTunnel->f_OnError(_Connection.m_Info.m_PeerAddress, "Error opening connection: {}"_f << OpenConnectionResult.f_GetExceptionStr());

					co_return OpenConnectionResult.f_GetException();
				}

				Connection.m_fSendData = fg_Move(*OpenConnectionResult);

				CAsyncSocketCallbacks SocketCallbacks;
				SocketCallbacks.m_fOnClose = g_ActorFunctor /
					[
						this
						, pTunnel
						, ConnectionID
						, PeerAddress = _Connection.m_Info.m_PeerAddress
						, AllowDestroy = g_AllowWrongThreadDestroy
					]
					(EAsyncSocketStatus _Reason, CStr &&_Message, EAsyncSocketCloseOrigin _Origin) -> TCFuture<void>
					{
						auto &Internal = *mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (pConnection)
						{
							auto DestroyFuture = pConnection->f_Destroy();
							Internal.m_Connections.f_Remove(pConnection);
							co_await fg_Move(DestroyFuture).f_Wrap() > fg_LogError("Network tunnel client", "Failed to destroy connection");
						}

						co_await pTunnel->f_OnClose(PeerAddress, _Message);

						co_return {};
					}
				;

				SocketCallbacks.m_fOnReceiveData = g_ActorFunctor / [this, ConnectionID, AllowDestroy = g_AllowWrongThreadDestroy]
					(TCSharedPointer<CSecureByteVector> &&_pMessage) -> TCFuture<void>
					{
						auto &Internal = *mp_pInternal;

						auto *pConnection = Internal.m_Connections.f_FindEqual(ConnectionID);
						if (!pConnection || !pConnection->m_fSendData)
							co_return DMibErrorInstance("Socket no longer exists");

						co_await pConnection->m_fSendData(fg_Move(*_pMessage));
						co_return {};
					}
				;

				auto SocketResult = co_await _Connection.f_Accept(fg_Move(SocketCallbacks)).f_Wrap();
				if (!SocketResult)
				{
					co_await pTunnel->f_OnError(_Connection.m_Info.m_PeerAddress, "Error accepting connection"_f << SocketResult.f_GetExceptionStr());

					co_return SocketResult.f_GetException();
				}
				Connection.m_Socket = fg_Move(*SocketResult);

				Cleanup->f_Clear();

				pTunnel->f_OnConnection(_Connection.m_Info.m_PeerAddress) > fg_LogError("Network tunnel client", "Failed to call on connection on tunnel");

				co_return {};
			}
		;

		Callbacks.m_fFailedConnection = g_ActorFunctor / [pTunnel, AllowDestroy = g_AllowWrongThreadDestroy](CAsyncSocketActor::CConnectionInfo &&_ConnectionInfo) mutable -> TCFuture<void>
			{
				co_await pTunnel->f_OnError(_ConnectionInfo.m_PeerAddress, "Connection failed: {}"_f << _ConnectionInfo.m_Error);

				co_return {};
			}
		;

		TCVector<CNetAddress> ListenAddresses;

		CNetAddressTCPv4 DefaultListenAddress(CNetAddressIPv4{127, 0, 0, 1}, 0);

		if (_ListenHost)
		{
			if (!Internal.m_AddressResolver)
				Internal.m_AddressResolver = NConcurrency::fg_ConstructActor<NNetwork::CResolveActor>();

			ListenAddresses.f_Insert(co_await Internal.m_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _ListenHost, NNetwork::ENetAddressType_TCPv4));
		}
		else
			ListenAddresses.f_Insert(DefaultListenAddress);

		auto ListenResults = co_await Internal.m_SocketServer
			(
				&CAsyncSocketServerActor::f_StartListenAddress
				, ListenAddresses
				, ENetFlag_None
				, fg_Move(Callbacks)
				, nullptr
			)
		;

		CNetAddress ReturnListenAddress;

		if (_ListenHost)
		{
			ReturnListenAddress = ListenAddresses[0];
			ReturnListenAddress.f_SetPort(ListenResults.m_ListenPorts[0]);
		}
		else
			ReturnListenAddress = CNetAddressTCPv4{DefaultListenAddress.f_GetIP(), ListenResults.m_ListenPorts[0]};

		pTunnel->m_ListenSubscription = fg_Move(ListenResults.m_Subscription);

		CleanupTunnel->f_Clear();
		co_return
			{
				g_ActorSubscription / [this, TunnelID]() -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;
					auto pTunnel = Internal.m_Tunnels.f_FindEqual(TunnelID);
					if (pTunnel)
					{
						TCFuture<void> DestroyFuture = (*pTunnel)->f_Destroy();
						Internal.m_Tunnels.f_Remove(TunnelID);
						co_await fg_Move(DestroyFuture);
					}

					co_return {};
				}
				, fg_Move(ReturnListenAddress)
			}
		;
	}

	TCFuture<void> CNetworkTunnelsClient::CInternal::f_Subscribe()
	{
		m_NetworkTunnelSubscription = co_await m_TrustManager->f_SubscribeTrustedActors<ICNetworkTunnels>();

		co_return {};
	}
}
