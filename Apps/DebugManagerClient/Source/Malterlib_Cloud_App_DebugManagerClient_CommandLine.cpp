// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

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
