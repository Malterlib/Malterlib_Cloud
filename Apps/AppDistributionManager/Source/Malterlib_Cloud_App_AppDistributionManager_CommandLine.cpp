// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
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

		auto AddOption_Tags = "Tags?"_=
			{
				"Names"_= {"--tags"}
				, "Type"_= COneOfType{CEJSON{""}, COneOf{false}}
				, "Description"_= "Distribute versions have all these these tags only.\n"
				"If you leave tags empty no versions will be distributed.\n"
			}
		;
		auto AddOption_Platforms = "Platforms?"_=
			{
				"Names"_= {"--platforms"}
				, "Type"_= COneOfType{CEJSON{""}, COneOf{false}}
				, "Description"_= "Distribute versions from these platforms.\n"
				"Leave empty to distribute all platforms.\n"
			}
		;
		auto AddOption_Branches = "BranchWildcards?"_=
			{
				"Names"_= {"--branches"}
				, "Type"_= {""}
				, "Description"_= "Distribute versions from these branches.\n"
				"Leave empty to allow any branch.\n"
				"Branches can be matched with wildcards.\n"
			}
		;
		auto AddOption_RenameTemplate = "RenameTemplate?"_=
			{
				"Names"_= {"--rename-template"}
				, "Type"_= ""
				, "Default"_= "{Name}/{PlatformFamily}/{Name}-{Version}.{FileExtension}"
				, "Description"_= "Template to use when renaming the package for distribution.\n"
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

		auto AddOption_DeployDestinations = "DeployDestinations?"_=
			{
				"Names"_= {"--deploy-destinations"}
				, "Type"_= {COneOf{"FileSystem"}}
				, "Description"_= "The deploy destinations to deploy to.\n"
				"Leave empty to determine deploy types from version meta data\n"
				"Supported types:\n"
				"@Indent=27\r"
				"   FileSystem              Deploy to the local file system. Useful for example for web distribution.\r"
				"\r"
			}
		;
		auto AddOption_VersionManagerApplication =
			{
				"Names"_= {"--application"}
				, "Type"_= ""
				, "Description"_= "The version manager application to distribute.\n"
			}
		;

		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_= {"--distribution-add"}
					, "Description"_= "Adds an application distribution.\n"
					, "Options"_=
					{
						"VersionManagerApplication"_= AddOption_VersionManagerApplication
						, "Distribution?"_=
						{
							"Names"_= {"--distribution"}
							, "Type"_= ""
							, "Description"_= "The unique name you give the distribution.\n"
							"Defaults to the name of the version manager application."
						}
						, AddOption_Tags
						, AddOption_Platforms
						, AddOption_Branches
						, AddOption_RenameTemplate
						, AddOption_DeployDestinations
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppDistributionManagerActor::fp_CommandLine_DistributionAdd, _Params, _pCommandLine);
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_= {"--distribution-change-settings"}
					, "Description"_= "Change settings for distribution.\n"
					, "Options"_= 
					{
						"Distribution"_=
						{
							"Names"_= {"--distribution"}
							, "Type"_= ""
							, "Description"_= "Unique name of the distribution to change settings for."
						}
						, "VersionManagerApplication?"_= AddOption_VersionManagerApplication
						, AddOption_Tags
						, AddOption_Platforms
						, AddOption_Branches
						, AddOption_RenameTemplateNoDefault
						, AddOption_DeployDestinations
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppDistributionManagerActor::fp_CommandLine_DistributionChangeSettings, _Params, _pCommandLine);
				}
			)
		;
		
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_= {"--distribution-list"}
					, "Description"_= "List distributions."
					, "Options"_=
					{
						"Verbose?"_= 
						{
							"Names"_= {"--verbose", "-v"}
							, "Default"_= false
							, "Description"_= "Display more extensive information about the distribution."
						}
						, "Distribution?"_=
						{
							"Names"_= {"--distribution"}
							, "Default"_= "" 
							, "Description"_= "Unique name of the distribution to list. Leave empty to list all distributions."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppDistributionManagerActor::fp_CommandLine_DistributionEnum, _Params, _pCommandLine);
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_= {"--distribution-remove"}
					, "Description"_= "Remove the distribution."
					, "Parameters"_=
					{
						"Distribution"_=
						{
							"Type"_= ""
							, "Description"_= "The name of the distribution to remove."
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppDistributionManagerActor::fp_CommandLine_DistributionRemove, _Params, _pCommandLine);
				}
			)
		;
		DistributionManagement.f_RegisterCommand
			(
				{
					"Names"_= {"--application-list-versions"}
					, "Description"_= "List versions available to distribute from."
					, "Options"_=
					{
						"Verbose?"_=
						{
							"Names"_= {"--verbose", "-v"}
							, "Default"_= false
							, "Description"_= "Display more extensive information about the versions."
						}
						, "Application?"_=
						{
							"Names"_= {"--application"}
							, "Default"_= ""
							, "Description"_= "The application to list versions for.\n"
							"Leave empty to list all applications.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppDistributionManagerActor::fp_CommandLine_ApplicationListAvailableVersions, _Params, _pCommandLine);
				}
			)
		;
	}
}
