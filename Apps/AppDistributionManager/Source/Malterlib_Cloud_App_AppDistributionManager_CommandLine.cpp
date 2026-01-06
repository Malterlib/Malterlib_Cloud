// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	void CAppDistributionManagerActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);

		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud App Distribution Manager"
				, "Manages distribution of malterlib cloud applications to third party app stores and directory structures."
			)
		;

		auto DistributionManagement = o_CommandLine.f_AddSection("Distribution Management", "Commands to manage AppDistributionManager distributions");

		auto AddOption_Tags = "Tags?"_o=
			{
				"Names"_o= _o["--tags"]
				, "Type"_o= _o[""]
				, "Description"_o= "Distribute versions have all these these tags only.\n"
				"If you leave tags empty no versions will be distributed.\n"
			}
		;
		auto AddOption_Platforms = "Platforms?"_o=
			{
				"Names"_o= _o["--platforms"]
				, "Type"_o= _o[""]
				, "Description"_o= "Distribute versions from these platforms.\n"
				"Leave empty to distribute all platforms.\n"
			}
		;
		auto AddOption_Branches = "BranchWildcards?"_o=
			{
				"Names"_o= _o["--branches"]
				, "Type"_o= _o[""]
				, "Description"_o= "Distribute versions from these branches.\n"
				"Leave empty to allow any branch.\n"
				"Branches can be matched with wildcards.\n"
			}
		;
		auto AddOption_RenameTemplate = "RenameTemplate?"_o=
			{
				"Names"_o= _o["--rename-template"]
				, "Type"_o= ""
				, "Default"_o= "{Name}/{PlatformFamily}/{Name}-{Version}.{FileExtension}"
				, "Description"_o= "Template to use when renaming the package for distribution.\n"
				"Leave empty to not rename distribution. Template variables:\n"
				"@Indent=27\r"
				"   {Name}                  The name of the distribution.\r"
				"   {VersionAppName}        The name of the version manager application.\r"
				"   {PlatformFull}          The distribution platform include architecture.\r"
				"   {Platform}              The distribution platform.\r"
				"   {PlatformFamily}        The distribution platform family (deduced by logic).\r"
				"   {PlatformArchitecture}  The distribution platform architecture (deduced by logic).\r"
				"   {Version}               The distribution version without branch.\r"
				"   {VersionBranch}         The distribution version branch.\r"
				"   {VersionMajor}          The distribution major version.\r"
				"   {VersionMinor}          The distribution minor version.\r"
				"   {VersionRevision}       The distribution revision version.\r"
				"   {File}                  The original file name of the distribution file.\r"
				"   {FileName}              The original file name without extension of the distribution file.\r"
				"   {FileExtension}         The original extension of the distribution file.\r"
				"\r"
			}
		;

		auto AddOption_RenameTemplateNoDefault = fg_TempCopy(AddOption_RenameTemplate);
		AddOption_RenameTemplateNoDefault.m_Value.f_RemoveMember("Default");

		auto AddOption_DeployDestinations = "DeployDestinations?"_o=
			{
				"Names"_o= _o["--deploy-destinations"]
				, "Type"_o= _o[COneOf{"FileSystem"}]
				, "Description"_o= "The deploy destinations to deploy to.\n"
				"Leave empty to determine deploy types from version metadata\n"
				"Supported types:\n"
				"@Indent=27\r"
				"   FileSystem              Deploy to the local file system. Useful for example for web distribution.\r"
				"\r"
			}
		;
		CEJsonOrdered AddOption_VersionManagerApplication
			{
				"Names"_o= _o["--application"]
				, "Type"_o= ""
				, "Description"_o= "The version manager application to distribute.\n"
			}
		;

		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--distribution-add"]
					, "Description"_o= "Adds an application distribution.\n"
					, "Options"_o=
					{
						"VersionManagerApplication"_o= AddOption_VersionManagerApplication
						, "Distribution?"_o=
						{
							"Names"_o= _o["--distribution"]
							, "Type"_o= ""
							, "Description"_o= "The unique name you give the distribution.\n"
							"Defaults to the name of the version manager application."
						}
						, AddOption_Tags
						, AddOption_Platforms
						, AddOption_Branches
						, AddOption_RenameTemplate
						, AddOption_DeployDestinations
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DistributionAdd(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--distribution-change-settings"]
					, "Description"_o= "Change settings for distribution.\n"
					, "Options"_o=
					{
						"Distribution"_o=
						{
							"Names"_o= _o["--distribution"]
							, "Type"_o= ""
							, "Description"_o= "Unique name of the distribution to change settings for."
						}
						, "VersionManagerApplication?"_o= AddOption_VersionManagerApplication
						, AddOption_Tags
						, AddOption_Platforms
						, AddOption_Branches
						, AddOption_RenameTemplateNoDefault
						, AddOption_DeployDestinations
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DistributionChangeSettings(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;

		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--distribution-list"]
					, "Description"_o= "List distributions."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the distribution."
						}
						, "Distribution?"_o=
						{
							"Names"_o= _o["--distribution"]
							, "Default"_o= ""
							, "Description"_o= "Unique name of the distribution to list. Leave empty to list all distributions."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DistributionEnum(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--distribution-remove"]
					, "Description"_o= "Remove the distribution."
					, "Parameters"_o=
					{
						"Distribution"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The name of the distribution to remove."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_DistributionRemove(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_o= _o["--application-list-versions"]
					, "Description"_o= "List versions available to distribute from."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= _o["--verbose", "-v"]
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the versions."
						}
						, "Application?"_o=
						{
							"Names"_o= _o["--application"]
							, "Default"_o= ""
							, "Description"_o= "The application to list versions for.\n"
							"Leave empty to list all applications.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_ApplicationListAvailableVersions(fg_Move(_Params), fg_Move(_pCommandLine));
				}
			)
		;
	}
}
