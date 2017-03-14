// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_LogAggregatorApp.h"

namespace NMib::NCloud::NLogAggregator
{
	void CLogAggregatorApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Log Aggregator"
				, "Aggregates logs from other apps." 
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		(void)DefaultSection;
	}
}
