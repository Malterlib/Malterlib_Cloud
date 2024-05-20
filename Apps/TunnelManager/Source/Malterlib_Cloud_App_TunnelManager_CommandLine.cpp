// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_TunnelManager_Internal.h"
#include "Malterlib_Cloud_App_TunnelManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>

namespace NMib::NCloud::NTunnelManager
{
	void CTunnelManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Tunnel Manager"
				, "Manages network tunnels." 
			)
		;
	}
}
