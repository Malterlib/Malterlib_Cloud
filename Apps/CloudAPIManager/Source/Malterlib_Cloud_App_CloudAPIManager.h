// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NCloudAPIManager
{
	struct CCloudAPIManagerDaemonActor : public CDistributedAppActor
	{
		CCloudAPIManagerDaemonActor();
		~CCloudAPIManagerDaemonActor();
		
		struct CServer;
		
	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override; 
		
		TCActor<CServer> mp_pServer;
	};
}
