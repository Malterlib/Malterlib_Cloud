// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/NetworkTunnel>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud
{
	struct CNetworkTunnelClient : public NConcurrency::CActor
	{
		CNetworkTunnelClient
			(
			 	NConcurrency::TCActor<NConcurrency::CActorDistributionManager> const &_DistributionManager
			 	, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
			)
		;
		~CNetworkTunnelClient();

		struct CTunnel
		{
			NConcurrency::CActorSubscription m_Subscription;
			NNetwork::CNetAddressTCPv4 m_ListenAddress;
		};

		NConcurrency::TCFuture<void> f_Start();

		NConcurrency::TCFuture<NContainer::TCMap<NStr::CStr, NContainer::TCMap<ICNetworkTunnel::CNetworkTunnelName, ICNetworkTunnel::CNetworkTunnel>>> f_EnumTunnels();

		NConcurrency::TCFuture<CTunnel> f_OpenTunnel
			(
			 	NStr::CStr const &_HostID
			 	, ICNetworkTunnel::CNetworkTunnelName const &_TunnelName
			 	, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NMib::NNetwork::CNetAddress const &_Address)> &&_fOnConnection
			 	, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NMib::NNetwork::CNetAddress const &_Address, NStr::CStr const &_Message)> &&_fOnClose
			 	, NConcurrency::TCActorFunctor<NConcurrency::TCFuture<void> (NMib::NNetwork::CNetAddress const &_Address, NStr::CStr const &_Error)> &&_fOnError
			)
		;

	private:
		NConcurrency::TCFuture<void> fp_Destroy() override;

		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
