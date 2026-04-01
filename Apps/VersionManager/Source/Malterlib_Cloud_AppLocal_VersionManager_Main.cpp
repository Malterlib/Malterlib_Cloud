// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_VersionManager.h"

using namespace NMib;
using namespace NMib::NCloud::NVersionManager;

struct CVersionManager : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudVersionManager"
				, "Malterlib Cloud Version Manager"
				, "Managers application distrbutions"
				, []
				{
					return fg_ConstructActor<CVersionManagerDaemonActor>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CVersionManager);
