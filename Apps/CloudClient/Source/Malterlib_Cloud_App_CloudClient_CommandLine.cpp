// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud Client"
				, "Runs commands on remote servers." 
			)
		;
		
		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"Timeout?"_= 
					{
						"Names"_= {"--timeout"}
						,"Default"_= 10.0
						, "Description"_= "The number of seconds to wait for remote servers to reply to commands"
					}
				}
			)
		;
		
		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"SelfUpdateCheck?"_= 
					{
						"Names"_= {"--self-update-check"}
						,"Default"_= true
						, "Description"_= "Check if a new version of the cloud client is available when running other commands."
					}
				}
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		(void)DefaultSection;
		
		fp_BackupManager_RegisterCommands(o_CommandLine.f_AddSection("Backup Manager", "Commands to control backup managers"));
		fp_VersionManager_RegisterCommands(o_CommandLine.f_AddSection("Version Manager", "Commands to control version managers"));
		
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--self-update"}
					, "Description"_= "Update cloud client from from connected version managers"
					, "Options"_=
					{
						"VersionManagerHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
							, "Description"_= "Only look for a new version on this version manager"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_SelfUpdate(_Params);
				}
			)
		;
	}
}
