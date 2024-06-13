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

			TCFuture<void> f_OnConnection(CCallbackInfo const &_CallbackInfo)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnConnection)
					co_return {};

				co_await m_fOnConnection(_CallbackInfo).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on connection on tunnel");

				co_return {};
			}

			TCFuture<void> f_OnClose(CCallbackInfo const &_CallbackInfo, CStr const &_Message)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnClose)
					co_return {};

				co_await m_fOnClose(_CallbackInfo, _Message).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on close on tunnel");

				co_return {};
			}

			TCFuture<void> f_OnError(CCallbackInfo const &_CallbackInfo, CStr const &_Error)
			{
				co_await ECoroutineFlag_AllowReferences;

				if (!m_fOnError)
					co_return {};

				co_await m_fOnError(_CallbackInfo, _Error).f_Wrap() > fg_LogError("Network tunnel client", "Failed to call on error on tunnel");

				co_return {};
			}

			DMibListLinkDS_Link(CTunnel, m_ByNameLink);
			ICNetworkTunnels::CNetworkTunnelName m_TunnelName;
			CStr m_HostID;
			TCDistributedActor<ICNetworkTunnels> m_TunnelActor;
			CActorSubscription m_ListenSubscription;
			TCActorFunctor<TCFuture<void> (CCallbackInfo const &_CallbackInfo)> m_fOnConnection;
			TCActorFunctor<TCFuture<void> (CCallbackInfo const &_CallbackInfo, CStr const &_Message)> m_fOnClose;
			TCActorFunctor<TCFuture<void> (CCallbackInfo const &_CallbackInfo, CStr const &_Error)> m_fOnError;
		};

		struct CNetworkTunnelsState
		{
			CActorSubscription m_ChangesSubscription;
			NContainer::TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel> m_Tunnels;
		};

		struct CByNameState
		{
			TCDistributedActor<ICNetworkTunnels> f_GetFirstAvailableTunnelActor(CStr const &_HostID);
			bool f_CanBeRemoved() const;

			DMibListLinkDS_List(CTunnel, m_ByNameLink) m_OpenedTunnels;
			TCSet<TCWeakDistributedActor<ICNetworkTunnels>> m_AvailableOnNetworkTunnels;
		};

		TCFuture<void> f_Subscribe();

		void f_RemoveTunnelByName(TCWeakDistributedActor<CActor> const &_Actor, CStr const &_Name);

		CNetworkTunnelsClient *m_pThis;
		TCActor<CActorDistributionManager> m_DistributionManager;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		TCTrustedActorSubscription<ICNetworkTunnels> m_NetworkTunnelSubscription;
		TCMap<TCWeakDistributedActor<CActor>, CNetworkTunnelsState> m_NetworkTunnelsState;
		TCActor<CAsyncSocketServerActor> m_SocketServer = fg_Construct();
		TCMap<mint, CConnection> m_Connections;
		TCMap<mint, TCSharedPointer<CTunnel>> m_Tunnels;
		TCMap<CStr, CByNameState> m_ByNameStates;
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

		{
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
		}

		co_await Internal.m_NetworkTunnelSubscription.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy tunnels client subscription");

		Internal.m_ByNameStates.f_Clear();

		{
			TCActorResultVector<void> Results;
			for (auto &State : Internal.m_NetworkTunnelsState)
			{
				if (State.m_ChangesSubscription)
					fg_Exchange(State.m_ChangesSubscription, nullptr)->f_Destroy() > Results.f_AddResult();
			}

			co_await Results.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy change subscriptions");
		}

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

	TCDistributedActor<ICNetworkTunnels> CNetworkTunnelsClient::CInternal::CByNameState::f_GetFirstAvailableTunnelActor(CStr const &_HostID)
	{
		for (auto &NewTunnel : m_AvailableOnNetworkTunnels)
		{
			auto LockedActor = NewTunnel.f_Lock();
			if (!LockedActor)
				continue;

			if (_HostID && LockedActor->f_GetHostInfo().m_RealHostID != _HostID)
				continue;

			return LockedActor;
		}

		return {};
	}

	bool CNetworkTunnelsClient::CInternal::CByNameState::f_CanBeRemoved() const
	{
		return m_AvailableOnNetworkTunnels.f_IsEmpty() && m_OpenedTunnels.f_IsEmpty();
	}

	auto CNetworkTunnelsClient::f_OpenTunnel(COpenTunnel &&_OpenTunnel) -> TCFuture<CTunnel>
	{
		auto &Internal = *mp_pInternal;

		auto CheckDestroy = co_await f_CheckDestroyedOnResume();

		mint TunnelID = ++Internal.m_TunnelID;
		TCSharedPointer<CInternal::CTunnel> pTunnel = fg_Construct();

		Internal.m_Tunnels[TunnelID] = pTunnel;
		auto TunnelSubscription = g_ActorSubscription / [this, TunnelID]() -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;
				auto pTunnel = Internal.m_Tunnels.f_FindEqual(TunnelID);
				if (!pTunnel)
					co_return {};

				auto &Tunnel = **pTunnel;

				if (Tunnel.m_ByNameLink.f_IsInList())
				{
					auto *pByNameState = Internal.m_ByNameStates.f_FindEqual(Tunnel.m_TunnelName);
					if (pByNameState)
					{
						pByNameState->m_OpenedTunnels.f_Remove(&Tunnel);
						if (pByNameState->f_CanBeRemoved())
							Internal.m_ByNameStates.f_Remove(pByNameState);
					}
				}

				TCFuture<void> DestroyFuture = Tunnel.f_Destroy();
				Internal.m_Tunnels.f_Remove(TunnelID);
				co_await fg_Move(DestroyFuture);

				co_return {};
			}
		;

		auto Cleanup = g_OnScopeExit / [&]
			{
				fg_Exchange(TunnelSubscription, nullptr)->f_Destroy() > fg_LogError("Network tunnel client", "Failed to destroy tunnel");
			}
		;

		pTunnel->m_TunnelName = _OpenTunnel.m_TunnelName;
		pTunnel->m_HostID = _OpenTunnel.m_HostID;
		pTunnel->m_fOnConnection = fg_Move(_OpenTunnel.m_fOnConnection);
		pTunnel->m_fOnClose = fg_Move(_OpenTunnel.m_fOnClose);
		pTunnel->m_fOnError = fg_Move(_OpenTunnel.m_fOnError);

		auto &ByNameState = Internal.m_ByNameStates[_OpenTunnel.m_TunnelName];
		ByNameState.m_OpenedTunnels.f_Insert(*pTunnel);

		pTunnel->m_TunnelActor = ByNameState.f_GetFirstAvailableTunnelActor(_OpenTunnel.m_HostID);

		if (!pTunnel->m_TunnelActor)
		{
			// This is for legacy tunnels that don't have support for subscribing to changes
			for (auto &Actor : Internal.m_NetworkTunnelSubscription.m_Actors)
			{
				if (Actor.m_Actor->f_InterfaceVersion() >= ICNetworkTunnels::EProtocolVersion_SupportSubscribeTunnels)
					continue;

				if (!_OpenTunnel.m_HostID || Actor.m_TrustInfo.m_HostInfo.m_HostID == _OpenTunnel.m_HostID)
				{
					pTunnel->m_TunnelActor = Actor.m_Actor;
					break;
				}
			}
		}

		if (!_OpenTunnel.m_bWaitForTunnel && !pTunnel->m_TunnelActor)
			co_return DMibErrorInstance("No tunnel subscription found for this host ID");

		CAsyncSocketServerCallbacks Callbacks;

		Callbacks.m_fNewConnection = g_ActorFunctor / [this, pTunnel, AllowDestroy = g_AllowWrongThreadDestroy](CAsyncSocketNewServerConnection &&_Connection) mutable -> TCFuture<void>
			{
				auto &Internal = *mp_pInternal;
				mint ConnectionID = ++Internal.m_ConnectionID;

				CCallbackInfo CallbackInfo{.m_Address = _Connection.m_Info.m_PeerAddress, .m_ConnectionID = CStr::fs_ToStr(ConnectionID)};

				if (!pTunnel->m_TunnelActor)
				{
					CStr Error = "Tunnel is not available";

					_Connection.f_Reject(Error);

					co_await pTunnel->f_OnError(CallbackInfo, Error);

					co_return DMibErrorInstance(Error);
				}

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

				CallbackInfo.m_RemoteHostID = pTunnel->m_TunnelActor->f_GetHostInfo().m_RealHostID;

				auto OpenConnectionResult = co_await pTunnel->m_TunnelActor.f_CallActor(&ICNetworkTunnels::f_OpenConnection)
					(
						ICNetworkTunnels::COpenConnection
						{
							.m_Name = pTunnel->m_TunnelName
							, .m_fOnReceive = g_ActorFunctor
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
							, .m_ConnectionID = CStr::fs_ToStr(ConnectionID)
						}
					)
					.f_Wrap()
				;

				if (!OpenConnectionResult)
				{
					co_await pTunnel->f_OnError(CallbackInfo, "Error opening connection: {}"_f << OpenConnectionResult.f_GetExceptionStr());

					co_return OpenConnectionResult.f_GetException();
				}

				Connection.m_fSendData = fg_Move(*OpenConnectionResult);

				CAsyncSocketCallbacks SocketCallbacks;
				SocketCallbacks.m_fOnClose = g_ActorFunctor /
					[
						this
						, pTunnel
						, ConnectionID
						, CallbackInfo
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

						co_await pTunnel->f_OnClose(CallbackInfo, _Message);

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
					co_await pTunnel->f_OnError(CallbackInfo, "Error accepting connection: {}"_f << SocketResult.f_GetExceptionStr());

					co_return SocketResult.f_GetException();
				}
				Connection.m_Socket = fg_Move(*SocketResult);

				Cleanup->f_Clear();

				pTunnel->f_OnConnection(CallbackInfo) > fg_LogError("Network tunnel client", "Failed to call on connection on tunnel");

				co_return {};
			}
		;

		Callbacks.m_fFailedConnection = g_ActorFunctor / [pTunnel, AllowDestroy = g_AllowWrongThreadDestroy](CAsyncSocketActor::CConnectionInfo &&_ConnectionInfo) mutable -> TCFuture<void>
			{
				co_await pTunnel->f_OnError({.m_Address = _ConnectionInfo.m_PeerAddress}, "Connection failed: {}"_f << _ConnectionInfo.m_Error);

				co_return {};
			}
		;

		TCVector<CNetAddress> ListenAddresses;

		CNetAddressTCPv4 DefaultListenAddress(CNetAddressIPv4{127, 0, 0, 1}, 0);

		if (_OpenTunnel.m_ListenHost)
		{
			if (!Internal.m_AddressResolver)
				Internal.m_AddressResolver = NConcurrency::fg_ConstructActor<NNetwork::CResolveActor>();

			ListenAddresses.f_Insert(co_await Internal.m_AddressResolver(&NNetwork::CResolveActor::f_Resolve, _OpenTunnel.m_ListenHost, NNetwork::ENetAddressType_TCPv4));
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

		if (_OpenTunnel.m_ListenHost)
		{
			ReturnListenAddress = ListenAddresses[0];
			ReturnListenAddress.f_SetPort(ListenResults.m_ListenPorts[0]);
		}
		else
			ReturnListenAddress = CNetAddressTCPv4{DefaultListenAddress.f_GetIP(), ListenResults.m_ListenPorts[0]};

		pTunnel->m_ListenSubscription = fg_Move(ListenResults.m_Subscription);

		Cleanup.f_Clear();

		co_return
			{
				fg_Move(TunnelSubscription)
				, fg_Move(ReturnListenAddress)
			}
		;
	}

	void CNetworkTunnelsClient::CInternal::f_RemoveTunnelByName(TCWeakDistributedActor<CActor> const &_Actor, CStr const &_Name)
	{
		auto *pByNameState = m_ByNameStates.f_FindEqual(_Name);
		if (!pByNameState)
			return;

		pByNameState->m_AvailableOnNetworkTunnels.f_Remove(_Actor);
		for (auto &Tunnel : pByNameState->m_OpenedTunnels)
		{
			if (Tunnel.m_TunnelActor != _Actor)
				continue;

			Tunnel.m_TunnelActor = pByNameState->f_GetFirstAvailableTunnelActor(Tunnel.m_HostID);
		}

		if (pByNameState->f_CanBeRemoved())
			m_ByNameStates.f_Remove(pByNameState);
	}

	TCFuture<void> CNetworkTunnelsClient::CInternal::f_Subscribe()
	{
		m_NetworkTunnelSubscription = co_await m_TrustManager->f_SubscribeTrustedActors<ICNetworkTunnels>();

		co_await m_NetworkTunnelSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<ICNetworkTunnels> const &_Tunnels, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					if (_Tunnels->f_InterfaceVersion() < ICNetworkTunnels::EProtocolVersion_SupportSubscribeTunnels)
						co_return {};

					ICNetworkTunnels::CSubscribeTunnels Subscribe;
					Subscribe.m_fOnTunnelChange = g_ActorFunctor / [this, Actor = _Tunnels.f_Weak(), HostID = _ActorInfo.m_HostInfo.m_HostID]
						(ICNetworkTunnels::CTunnelChange &&_TunnelChange) -> TCFuture<void>
						{
							auto LockedActor = Actor.f_Lock();
							if (!LockedActor)
								co_return {};

							auto *pState = m_NetworkTunnelsState.f_FindEqual(Actor);
							if (!pState)
								co_return {};

							auto fRemoveByName = [&](CStr const &_TunnelName)
								{
									f_RemoveTunnelByName(Actor, _TunnelName);
								}
							;

							auto fAddByName = [&](CStr const &_TunnelName)
								{
									auto &ByNameState = m_ByNameStates[_TunnelName];

									ByNameState.m_AvailableOnNetworkTunnels[Actor];

									for (auto &Tunnel : ByNameState.m_OpenedTunnels)
									{
										if (Tunnel.m_HostID && HostID != Tunnel.m_HostID)
											continue;

										Tunnel.m_TunnelActor = LockedActor;
									}
								}
							;

							switch (_TunnelChange.f_GetTypeID())
							{
							case ICNetworkTunnels::ETunnelChange_Initial:
								{
									auto &Change = _TunnelChange.f_Get<ICNetworkTunnels::ETunnelChange_Initial>();

									for (auto &Name : pState->m_Tunnels.f_Keys())
										fRemoveByName(Name);

									pState->m_Tunnels = fg_Move(Change.m_Tunnels);

									for (auto &Name : pState->m_Tunnels.f_Keys())
										fAddByName(Name);
								}
								break;
							case ICNetworkTunnels::ETunnelChange_Add:
								{
									auto &Change = _TunnelChange.f_Get<ICNetworkTunnels::ETunnelChange_Add>();

									pState->m_Tunnels[Change.m_TunnelName] = fg_Move(Change.m_Tunnel);

									fAddByName(Change.m_TunnelName);
								}
								break;
							case ICNetworkTunnels::ETunnelChange_Remove:
								{
									auto &Change = _TunnelChange.f_Get<ICNetworkTunnels::ETunnelChange_Remove>();

									fRemoveByName(Change.m_TunnelName);

									pState->m_Tunnels.f_Remove(Change.m_TunnelName);
								}
								break;
							}

							co_return {};
						}
					;

					auto &State = m_NetworkTunnelsState[_Tunnels];
					State.m_ChangesSubscription = co_await _Tunnels.f_CallActor(&ICNetworkTunnels::f_SubscribeTunnels)(fg_Move(Subscribe));

					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> const &_Actor, CTrustedActorInfo &&_ActorInfo) -> TCFuture<void>
				{
					auto pState = m_NetworkTunnelsState.f_FindEqual(_Actor);
					if (!pState)
						co_return {};

					auto ChangesSubscription = fg_Move(pState->m_ChangesSubscription);

					for (auto &Name : pState->m_Tunnels.f_Keys())
						f_RemoveTunnelByName(_Actor, Name);

					m_NetworkTunnelsState.f_Remove(pState);

					if (ChangesSubscription)
						co_await fg_Move(ChangesSubscription)->f_Destroy();

					co_return {};
				}
				, "NetworkTunnelsClient"
				, "Failed to handle '{}' for network tunnel subscription"
			)
		;

		co_return {};
	}
}
