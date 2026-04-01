// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_DebugManagerClient.h"

using namespace NMib;
using namespace NMib::NCloud::NDebugManagerClient;

struct CDebugManagerClientApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudDebugManagerClient"
				, "Malterlib Build Manager Client"
				, "Servers symbols locally."
				, []
				{
					return fg_ConstructActor<CDebugManagerClientApp>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CDebugManagerClientApplication);
