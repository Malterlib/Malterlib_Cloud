
#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>

namespace NMib::NCloud::NBackupManager
{
	struct CBackupManagerServer;

	struct CBackupManagerApp : public CDistributedAppActor
	{
		CBackupManagerApp();
		~CBackupManagerApp();

	private:
		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		TCActor<CBackupManagerServer> mp_Server;
	};
}
