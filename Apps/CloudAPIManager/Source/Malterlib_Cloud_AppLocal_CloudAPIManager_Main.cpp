// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_CloudAPIManager.h"

using namespace NMib;
using namespace NMib::NCloud::NCloudAPIManager;

struct CCloudAPIManager : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudCloudAPIManager"
				, "Malterlib Cloud Cloud API Manager"
				, "Managers application distrbutions"
				, []
				{
					return fg_ConstructActor<CCloudAPIManagerDaemonActor>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CCloudAPIManager);
