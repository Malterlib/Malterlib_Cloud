// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_AppLocal_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppLocalActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CCloudClientAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"SelfUpdateCheck?"_o=
					{
						"Names"_o= {"--self-update-check"}
						, "Default"_o= true
						, "ValidForDirectCommand"_o= false
						, "Description"_o= "Check if a new version of the cloud client is available when running other commands."
					}
				}
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		(void)DefaultSection;
		
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_o= {"--self-update"}
					, "Description"_o= "Update cloud client from from connected version managers."
					, "Options"_o=
					{
						"VersionManagerHost?"_o=
						{
							"Names"_o= {"--host"}
							, "Default"_o= ""
							, "Description"_o= "Only look for a new version on this version manager."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CCloudClientAppLocalActor::fp_CommandLine_SelfUpdate, _Params, _pCommandLine);
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}
}
