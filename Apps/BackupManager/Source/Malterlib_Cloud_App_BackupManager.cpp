
#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	CBackupManagerApp::CBackupManagerApp()
		: CDistributedAppActor(CDistributedAppActor_Settings{"BackupManager"})
	{
	}
	
	CBackupManagerApp::~CBackupManagerApp()
	{
	}

	TCFuture<void> CBackupManagerApp::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		mp_Server = fg_ConstructActor<CBackupManagerServer>(fg_Construct(self), mp_State);

		co_await mp_Server(&CBackupManagerServer::f_Init);

		co_return {};
	}
	
	TCFuture<void> CBackupManagerApp::fp_StopApp()
	{	
		if (mp_Server)
		{
			DMibLogWithCategory(Mib/Cloud/BackupManager/Daemon, Info, "Shutting down");
			
			auto Result = co_await fg_Move(mp_Server).f_Destroy().f_Wrap();

			if (!Result)
				DMibLogWithCategory(Mib/Cloud/BackupManager/Daemon, Error, "Failed to shut down server: {}", Result.f_GetExceptionStr());
		}
		
		co_return {};
	}
}

namespace NMib::NCloud
{
	TCActor<CDistributedAppActor> fg_ConstructApp_BackupManager()
	{
		return fg_Construct<NBackupManager::CBackupManagerApp>();
	}
}
