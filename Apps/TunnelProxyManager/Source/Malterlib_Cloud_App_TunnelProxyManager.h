// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCActor<CNetworkTunnelsServer> mp_TunnelsServer;
		TCActor<CNetworkTunnelsClient> mp_TunnelsClient;
		TCVector<CActorSubscription> mp_TunnelPublicationSubscriptions;
		TCVector<CNetworkTunnelsClient::CTunnel> mp_Tunnels;
	};
}
