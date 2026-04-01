// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_CloudManager.h"

using namespace NMib;
using namespace NMib::NCloud::NCloudManager;

struct CCloudManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudCloudManager"
				, "Malterlib Cloud Manager"
				, "Manages cloud apps."
				, []
				{
					return fg_ConstructActor<CCloudManagerApp>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CCloudManagerApplication);
