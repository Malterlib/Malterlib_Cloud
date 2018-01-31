// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretsManagerDaemonActor : public CDistributedAppActor
	{
		CSecretsManagerDaemonActor();
		~CSecretsManagerDaemonActor();
		
		struct CServer;
		struct CServerController;
		
	private:
		TCContinuation<void> fp_StartApp(CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

#if DMibConfig_Tests_Enable
		TCContinuation<CEJSON> fp_Test_Command(CStr const &_Command, CEJSON const &_Params) override;
#endif

		TCActor<CServerController> mp_pServerController;
	};
}
