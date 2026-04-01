// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_DebugManager.h"

using namespace NMib;
using namespace NMib::NCloud::NDebugManager;

struct CDebugManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudDebugManager"
				, "Malterlib Debug Manager"
				, "Stores symbols, executabels and crash dumps, and serves them."
				, []
				{
					return fg_ConstructActor<CDebugManagerApp>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CDebugManagerApplication);
