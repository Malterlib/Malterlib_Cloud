// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_TunnelProxyManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Encoding/JSONShortcuts>

namespace NMib::NCloud::NTunnelProxyManager
{
	void CTunnelProxyManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Manager"
				, "Manages cloud applications." 
			)
		;
	}
}
