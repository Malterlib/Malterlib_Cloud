// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/NetworkTunnelsClient>
#include <Mib/Cloud/NetworkTunnelsServer>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NTunnelProxyManager
{
	struct CTunnelProxyManagerServer;

	struct CTunnelProxyManagerApp : public CDistributedAppActor
	{
		CTunnelProxyManagerApp();
		~CTunnelProxyManagerApp();

	private:
		struct CPublication
		{
			CActorSubscription m_Subscription;
			CStr m_Host;
		};

		struct CSubscription
		{
			CNetworkTunnelsClient::CTunnel m_Tunnel;
			CStr m_ListenHost;
		};

		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_ReloadConfig(TCActorFunctor<TCFuture<void> (CStr _Message)> _fLog);

		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCActor<CNetworkTunnelsServer> mp_TunnelsServer;
		TCActor<CNetworkTunnelsClient> mp_TunnelsClient;
		TCMap<CStr, CPublication> mp_Publications;
		TCMap<CStr, CSubscription> mp_Subscriptions;
	};
}
