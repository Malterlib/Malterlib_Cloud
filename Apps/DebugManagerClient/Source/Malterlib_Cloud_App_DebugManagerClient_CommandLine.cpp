// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_DebugManagerClient.h"

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Network/SSL>
#include <Mib/Web/WebSocket>

namespace NMib::NCloud::NDebugManagerClient
{
	void CDebugManagerClientApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Debug Manager Client"
				, "Servers symbols locally."
			)
		;
	}
}
