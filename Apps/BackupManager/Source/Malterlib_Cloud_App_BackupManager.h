
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
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override; 
		TCActor<CBackupManagerServer> mp_pServer;
	};
}
