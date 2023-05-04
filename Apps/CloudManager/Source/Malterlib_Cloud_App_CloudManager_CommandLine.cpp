// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Encoding/JSONShortcuts>

namespace NMib::NCloud::NCloudManager
{
	void CCloudManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Manager"
				, "Manages cloud applications." 
			)
		;

		auto DefaultSection = o_CommandLine.f_GetDefaultSection();

		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--dump-database"}
					, "Description"_= "Dump Cloud Manager specific database entries."
					, "Options"_=
					{
						"Prefix?"_=
						{
							"Names"_= {"--prefix", "-p"}
							, "Default"_= ""
							, "Description"_= "Limit output to tabels with prefix."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine) -> TCFuture<uint32>
				{
					co_await mp_Server(&CCloudManagerServer::f_DumpDatabaseEntries, _pCommandLine, _Params["Prefix"].f_String());

					co_return 0;
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}
}
