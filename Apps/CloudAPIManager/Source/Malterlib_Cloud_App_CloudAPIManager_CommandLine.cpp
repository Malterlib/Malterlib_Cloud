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

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	void CCloudAPIManagerDaemonActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Cloud API Manager"
				, "Manages updates for Malterlib cloud apps." 
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--cloud-api-test"}
					, "Description"_= "Test cloud api.\n"
				}
				, [this](NEncoding::CEJSON const &_Params) -> TCContinuation<CDistributedAppCommandLineResults>
				{
					TCContinuation<CDistributedAppCommandLineResults> Continuation;
					mp_pServer(&CServer::f_CommandLine_TestCloudAPI, _Params) > Continuation;
					return Continuation;
				}
			)
		;
	}
}
