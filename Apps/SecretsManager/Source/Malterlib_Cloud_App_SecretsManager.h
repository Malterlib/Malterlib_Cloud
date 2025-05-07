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
		TCFuture<void> fp_StartApp(CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

#if DMibConfig_Tests_Enable
		TCFuture<CEJsonSorted> fp_Test_Command(CStr _Command, CEJsonSorted const _Params) override;
#endif

		TCActor<CServerController> mp_pServerController;
	};
}
