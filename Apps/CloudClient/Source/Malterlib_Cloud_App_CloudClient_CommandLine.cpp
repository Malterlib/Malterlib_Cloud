// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JsonShortcuts>

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
					"Timeout?"_o=
					{
						"Names"_o= _o["--timeout"]
						, "Default"_o= 120.0
						, "ValidForDirectCommand"_o= false
						, "Description"_o= "The number of seconds to wait for remote servers to reply to commands."
					}
				}
			)
		;

		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		(void)DefaultSection;

		fp_BackupManager_RegisterCommands(o_CommandLine.f_AddSection("Backup Manager", "Commands to control backup managers."));
		fp_VersionManager_RegisterCommands(o_CommandLine.f_AddSection("Version Manager", "Commands to control version managers."));
		fp_SecretsManager_RegisterCommands(o_CommandLine.f_AddSection("Secrets Manager", "Commands to control secrets managers."));
		fp_NetworkTunnel_RegisterCommands(o_CommandLine.f_AddSection("Network Tunnel", "Commands to use network tunnels."));
		fp_DebugManager_RegisterCommands(o_CommandLine.f_AddSection("Debug Manager", "Commands to control debug managers."));
		fp_CloudManager_RegisterCommands(o_CommandLine.f_AddSection("Cloud Manager", "Commands to control cloud managers."));
		fp_Bootstrap_RegisterCommands(o_CommandLine.f_AddSection("Bootstrap", "Commands to setup a new cloud environment."));
	}
}
