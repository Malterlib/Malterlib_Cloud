
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
		: CDistributedAppActor(CDistributedAppActor_Settings{"BackupManager", false})
	{
	}
	
	CBackupManagerApp::~CBackupManagerApp()
	{
	}

	TCContinuation<void> CBackupManagerApp::fp_StartApp(NEncoding::CEJSON const &_Params)
	{
		TCContinuation<void> Continuation;
		mp_pServer = fg_ConstructActor<CBackupManagerServer>(fg_Construct(self), mp_State);
		mp_pServer(&CBackupManagerServer::f_Init) > Continuation;
		return Continuation;
	}
	
	TCContinuation<void> CBackupManagerApp::fp_StopApp()
	{	
		TCSharedPointer<CCanDestroyTracker> pCanDestroy = fg_Construct();
		
		if (mp_pServer)
		{
			DMibLogWithCategory(Mib/Cloud/BackupManager/Daemon, Info, "Shutting down");
			
			mp_pServer->f_Destroy() > [pCanDestroy](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/BackupManager/Daemon, Error, "Failed to shut down server: {}", _Result.f_GetExceptionStr());
				}
			;
			mp_pServer = nullptr;
		}
		
		return pCanDestroy->m_Continuation;
	}
}
