// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/File/File>

#include "Malterlib_Cloud_App_CloudClient.h"
#include "Bootstrap/Malterlib_Cloud_Bootstrap.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_Bootstrap_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--bootstrap-init"]
					, "Description"_o= "Initialize a new cloud environment.\n"
						"This command sets up the initial infrastructure needed for a Malterlib cloud deployment."
					, "Options"_o=
					{
						"Provider?"_o=
						{
							"Names"_o= _o["--provider"]
							, "Default"_o= "Aws"
							, "Description"_o= "The cloud provider to use. Currently supported: Aws"
						}
						, "Region?"_o=
						{
							"Names"_o= _o["--region"]
							, "Default"_o= ""
							, "Description"_o= "The cloud region to deploy to (e.g., us-east-1 for AWS)."
						}
						, "Environment?"_o=
						{
							"Names"_o= _o["--environment"]
							, "Default"_o= ""
							, "Description"_o= "The environment name (e.g., production, staging, development)."
						}
						, "AwsAccessKeyId?"_o=
						{
							"Names"_o= _o["--aws-access-key-id"]
							, "Default"_o= fg_GetSys()->f_GetEnvironmentVariable("AWS_ACCESS_KEY_ID", "")
							, "Description"_o= "AWS Access Key ID (defaults to AWS_ACCESS_KEY_ID env var)."
						}
						, "AwsSecretAccessKey?"_o=
						{
							"Names"_o= _o["--aws-secret-access-key"]
							, "Default"_o= fg_GetSys()->f_GetEnvironmentVariable("AWS_SECRET_ACCESS_KEY", "")
							, "Description"_o= "AWS Secret Access Key (defaults to AWS_SECRET_ACCESS_KEY env var)."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					NStr::CStr RootDir = NFile::CFile::fs_AppendPath(mp_State.m_RootDirectory, "Bootstrap");
					return NBootstrap::fg_Bootstrap_MalterlibCloud(fg_Move(_Params), fg_Move(_pCommandLine), RootDir, mp_State.m_TrustManager);
				}
			)
		;
	}
}
