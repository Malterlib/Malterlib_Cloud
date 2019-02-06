
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NBackupManager
{
	TCFuture<void> CBackupManagerServer::fp_Publish()
	{
		return mp_ProtocolInterface.f_Publish<CBackupManager>(mp_AppState.m_DistributionManager, this, CBackupManager::mc_pDefaultNamespace);
	}
}
