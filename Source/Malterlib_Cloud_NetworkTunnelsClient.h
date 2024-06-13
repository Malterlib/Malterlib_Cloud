// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/NetworkTunnels>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud
{
	struct CNetworkTunnelsClient : public NConcurrency::CActor
	{
		CNetworkTunnelsClient
			(
				NConcurrency::TCActor<NConcurrency::CActorDistributionManager> const &_DistributionManager
				, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
			)
		;
		~CNetworkTunnelsClient();

		struct CTunnel
		{
			NConcurrency::CActorSubscription m_Subscription;
			NNetwork::CNetAddress m_ListenAddress;
		};

		struct CCallbackInfo
		{
			NMib::NNetwork::CNetAddress m_Address;
			NStr::CStr m_RemoteHostID;
			NStr::CStr m_ConnectionID;
		};

		struct COpenTunnel
		{
			NStr::CStr m_HostID; // Leave empty to allow any host
			ICNetworkTunnels::CNetworkTunnelName m_TunnelName;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (CCallbackInfo const &_CallbackInfo)> m_fOnConnection;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (CCallbackInfo const &_CallbackInfo, NStr::CStr const &_Message)> m_fOnClose;
			NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (CCallbackInfo const &_CallbackInfo, NStr::CStr const &_Error)> m_fOnError;
			NStr::CStr m_ListenHost;
			bool m_bWaitForTunnel = false;
		};

		NConcurrency::TCFuture<void> f_Start();

		NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, NContainer::TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> f_EnumTunnels();

		NConcurrency::TCFuture<CTunnel> f_OpenTunnel(COpenTunnel &&_OpenTunnel);

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
