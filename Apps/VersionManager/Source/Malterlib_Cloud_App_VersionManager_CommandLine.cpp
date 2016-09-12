// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Process/StdIn>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_VersionManager.h"

namespace NMib::NCloud::NVersionManager
{
	void CVersionManagerDaemonActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Favro Cloud Version Manager"
				, "Manages updates for Malterlib cloud apps." 
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		(void)DefaultSection;
	}
}
