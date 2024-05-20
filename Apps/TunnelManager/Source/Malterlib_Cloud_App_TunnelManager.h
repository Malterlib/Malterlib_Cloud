// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NTunnelManager
{
	struct CTunnelManagerServer;
	
	struct CTunnelManagerApp : public CDistributedAppActor
	{
		CTunnelManagerApp();
		~CTunnelManagerApp();
		
	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCActor<CTunnelManagerServer> mp_Server;
	};
}
