// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_DebugManagerClient.h"

using namespace NMib;
using namespace NMib::NCloud::NDebugManagerClient;

class CDebugManagerClientApplication : public CApplication
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
