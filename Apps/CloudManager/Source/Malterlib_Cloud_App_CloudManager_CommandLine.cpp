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
					"Names"_o= _o["--dump-database"]
					, "Description"_o= "Dump Cloud Manager specific database entries."
					, "Options"_o=
					{
						"Prefix?"_o=
						{
							"Names"_o= _o["--prefix", "-p"]
							, "Default"_o= ""
							, "Description"_o= "Limit output to tabels with prefix."
						}
					}
				}
				, [this](CEJSONSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine) -> TCFuture<uint32>
				{
					co_await mp_Server(&CCloudManagerServer::f_DumpDatabaseEntries, _pCommandLine, _Params["Prefix"].f_String());

					co_return 0;
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}
}
