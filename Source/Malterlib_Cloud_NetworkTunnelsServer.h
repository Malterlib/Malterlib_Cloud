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
	struct CNetworkTunnelsServer : public NConcurrency::CActor
	{
		CNetworkTunnelsServer
			(
				NConcurrency::TCActor<NConcurrency::CActorDistributionManager> const &_DistributionManager
				, NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> const &_TrustManager
				, NFunction::TCFunctionMovable<NConcurrency::CDistributedAppAuditor (NConcurrency::CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> &&_AuditorFactory
				, NStr::CStr const &_LogCategory
				, NStr::CStr const &_PermissionPrefix
			)
		;
		~CNetworkTunnelsServer();

		NConcurrency::TCFuture<NConcurrency::CActorSubscription> f_PublishNetworkTunnel
			(
				ICNetworkTunnels::CNetworkTunnelName _Name
				, NStr::CStr _Host
				, uint16 _Port
				, NEncoding::CEJsonSorted _Metadata
			)
		;

		NConcurrency::TCFuture<void> f_Start();

	private:
		struct CInternal;

		NConcurrency::TCFuture<void> fp_Destroy() override;

		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
