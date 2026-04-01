// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Core/Application>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedDaemon>

#include "Malterlib_Cloud_App_TunnelProxyManager.h"

using namespace NMib;
using namespace NMib::NCloud::NTunnelProxyManager;

struct CTunnelProxyManagerApplication : public CApplication
{
	aint f_Main()
	{
		NConcurrency::CDistributedDaemon Daemon
			{
				"MalterlibCloudTunnelProxyManager"
				, "Malterlib Tunnel Proxy Manager"
				, "Manages incoming and outgoing tunnels."
				, []
				{
					return fg_ConstructActor<CTunnelProxyManagerApp>();
				}
			}
		;
		return Daemon.f_Run();
	}
};

DAppImplement(CTunnelProxyManagerApplication);
