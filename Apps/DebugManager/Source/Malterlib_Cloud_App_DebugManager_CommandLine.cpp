// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_DebugManager.h"

#include <Mib/CrashReport/BuildID>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Network/SSL>
#include <Mib/Web/WebSocket>

namespace NMib::NCloud::NDebugManager
{
	void CDebugManagerApp::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Debug Manager"
				, "Manages debug information and crash dumps."
			)
		;

		auto Section = o_CommandLine.f_GetDefaultSection();

		Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--test-detection"]
					, "Description"_o= "Adds an application distribution.\n"
					, "Options"_o=
					{
						"WorkingDirectory"_o=
						{
							"Names"_o= _o["--working-directory"]
							, "Default"_o= NFile::CFile::fs_GetCurrentDirectory()
							, "Description"_o= "Temp"
						}
					}
					, "Parameters"_o=
					{
						"File"_o=
						{
							"Type"_o= ""
							, "Description"_o= "File."
						}
					}
				}
				, [](CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine) -> TCFuture<uint32>
				{
					auto BuildID = co_await NCrashReport::fg_GetBuildIDsFromFile(CFile::fs_GetExpandedPath(_Params["File"].f_String(), _Params["WorkingDirectory"].f_String()));

					*_pCommandLine += "BuildID: {}\n"_f << BuildID;

					co_return 0;
				}
			)
		;
	}
}
