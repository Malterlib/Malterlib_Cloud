// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_BackupManager.h"

using namespace NMib;
using namespace NMib::NCloud::NBackupManager;

struct CBackupManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudBackupManager"
				, "Malterlib Cloud Backup Manager"
				, "Manages backups"
				, []
				{
					return fg_ConstructActor<CBackupManagerApp>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CBackupManagerApplication);
