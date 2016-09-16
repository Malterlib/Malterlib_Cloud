// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine)
	{
		CDistributedAppActor::fp_BuildCommandLine(o_CommandLine);
		o_CommandLine.f_SetProgramDescription
			(
				"Malterlib Cloud App Manager"
				, "Manages malterlib cloud applications by providing services such as encryption at rest and automatic updates." 
			)
		;
		
		auto DefaultSection = o_CommandLine.f_GetDefaultSection();
		
		auto SettingsOption_Executable = "Executable?"_= 
			{
				"Names"_= {"--executable"}
				,"Type"_= ""
				, "Description"_= "Start this executable when running application.\n"
					"Can contain sub-path.\n"
			}
		;
		auto SettingsOption_RunAsUser = "RunAsUser?"_=
			{
				"Names"_= {"--run-as-user"}
				, "Type"_= ""
				, "Description"_= "Run the application as this user."
			}
		;
		auto SettingsOption_RunAsGroup = "RunAsGroup?"_= 
			{
				"Names"_= {"--run-as-group"}
				, "Type"_= ""
				, "Description"_= "Run the application as this group."
			}
		;
		
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-add"}
					, "Description"_= 
						"Adds an application.\n"
						"By default the application will run as root."
					, "Options"_= 
					{
						"Name"_= 
						{
							"Names"_= {"--name"}
							,"Type"_= ""
							, "Description"_= "Uniquely name the application."
						}
						, "EncryptionStorage?"_= 
						{
							"Names"_= {"--encryption-storage"}
							, "Default"_= ""
							, "Description"_= "Select the file or device that should be the storage for encryption."
						}
						, "Version?"_= 
						{
							"Names"_= {"--version"}
							,"Type"_= "" 
							, "Description"_= "The version to install from version manager.\n"
								"Defaults to the latest version available.\n"
						}
						, "ForceOverwrite?"_= 
						{
							"Names"_= {"--force-overwrite"}
							,"Default"_= false 
							, "Description"_= "Force zfs to overwrite storage"
						}
						, SettingsOption_Executable
						, "ExecutableParameters?"_= 
						{
							"Names"_= {"--executable-parameters"}
							, "Default"_= {"--daemon-run"}
							, "Type"_= {""} 
							, "Description"_= "Run the application with these executable parameters."
						}
						, SettingsOption_RunAsUser
						, SettingsOption_RunAsGroup
						, "AutoUpdateTags?"_= 
						{
							"Names"_= {"--auto-update-tags"}
							, "Default"_= false
							, "Type"_= COneOfType{{{""}, COneOf{false}}} 
							, "Description"_= "Auto update the application when new versions become available that has all these these tags."
						}
						, "AutoUpdateBranches?"_= 
						{
							"Names"_= {"--auto-update-branches"}
							, "Default"_= _[_]
							, "Type"_= {""} 
							, "Description"_= "Auto update the application only for versions from these branches.\n"
							"Leave empty to allow any branch\n"
						}
						, "UpdateScript_PreUpdate?"_= 
						{
							"Names"_= {"--update-script-pre-update"}
							, "Default"_= ""
							, "Description"_= "Set a script to run pre update.\n"
						}
						, "UpdateScript_PostUpdate?"_= 
						{
							"Names"_= {"--update-script-post-update"}
							, "Default"_= ""
							, "Description"_= "Set a script to run post update.\n"
						}
						, "UpdateScript_PostLaunch?"_= 
						{
							"Names"_= {"--update-script-post-launch"}
							, "Default"_= ""
							, "Description"_= "Set a script to run post launch.\n"
						}
					}
					, "Parameters"_=
					{
						"Package"_=  
						{
							"Type"_= ""
							, "Description"_= "The files needed to run the application.\n"
							"Can be a version manager application name, a directory, or a tar.gz file. Will look for version manager applications"
							"first. If not found it will assume that the name is a file and look for it on disk."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_AddApplication(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-change-settings"}
					, "Description"_= "Change settings for application.\n"
					, "Options"_= 
					{
						"Name"_= 
						{
							"Names"_= {"--name"}
							,"Type"_= ""
							, "Description"_= "Unique name of the application to change settings for."
						}
						, SettingsOption_Executable
						, "ExecutableParameters?"_= 
						{
							"Names"_= {"--executable-parameters"}
							, "Type"_= {""} 
							, "Description"_= "Run the application with these executable parameters."
						}				
						, SettingsOption_RunAsUser
						, SettingsOption_RunAsGroup
						, "AutoUpdateTags?"_= 
						{
							"Names"_= {"--auto-update-tags"}
							, "Type"_= COneOfType{{{""}, COneOf{false}}} 
							, "Description"_= "Auto update the application when new versions become available that has all these these tags."
						}
						, "AutoUpdateBranches?"_= 
						{
							"Names"_= {"--auto-update-branches"}
							, "Type"_= {""} 
							, "Description"_= "Auto update the application only for versions from these branches.\n"
							"Leave empty to allow any branch\n"
						}
						, "VersionManagerApplication?"_= 
						{
							"Names"_= {"--version-manager-application"}
							, "Type"_= {""} 
							, "Description"_= "Get updates from the version manager application with this name."
						}				
						, "UpdateFromVersionInfo?"_= 
						{
							"Names"_= {"--update-from-version-info"}
							, "Default"_= false 
							, "Description"_= "Update settings from the last installed version manager application info."
						}				
						, "Force?"_= 
						{
							"Names"_= {"--force"}
							, "Default"_= false 
							, "Description"_= "Force running the update process even if no settings are changed."
						}				
						, "UpdateScript_PreUpdate?"_= 
						{
							"Names"_= {"--update-script-pre-update"}
							, "Type"_= ""
							, "Description"_= "Set a script to run pre update.\n"
						}
						, "UpdateScript_PostUpdate?"_= 
						{
							"Names"_= {"--update-script-post-update"}
							, "Type"_= ""
							, "Description"_= "Set a script to run post update.\n"
						}
						, "UpdateScript_PostLaunch?"_= 
						{
							"Names"_= {"--update-script-post-launch"}
							, "Type"_= ""
							, "Description"_= "Set a script to run post launch.\n"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_ChangeApplicationSettings(_Params);
				}
			)
		;
		
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-list"}
					, "Description"_= "List applications"
					, "Options"_=
					{
						"Verbose?"_= 
						{
							"Names"_= {"--verbose", "-v"}
							, "Default"_= false
							, "Description"_= "Display more extensive information about the applications." 
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_EnumApplications(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-list-versions"}
					, "Description"_= "List versions available to update to."
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
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_ListAvailableVersions(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-remove"}
					, "Description"_= "Remove the application"
					, "Parameters"_=
					{
						"Name"_= 
						{
							"Type"_= ""
							, "Description"_= "The name of the application to remove"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_RemoveApplication(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-update-from-file"}
					, "Description"_= "Update the application package from file"
					, "Options"_= 
					{
						"Name"_= 
						{
							"Names"_= {"--name"}
							,"Type"_= ""
							, "Description"_= "Unique name of the application to update."
						}
					}
					, "Parameters"_=
					{
						"Package"_=  
						{
							"Type"_= ""
							, "Description"_= "The files needed to run the application.\n"
							"Can be a directory, or a tar.gz file."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_UpdateApplication(_Params, false);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-update"}
					, "Description"_= "Update the application package from version manager"
					, "Options"_= 
					{
						"Name"_= 
						{
							"Names"_= {"--name"}
							,"Type"_= ""
							, "Description"_= "Unique name of the application to update."
						}
						, "DryRun?"_= 
						{
							"Names"_= {"--dry-run"}
							,"Default"_= false
							, "Description"_= "Only list action to take, don't actually do the update."
						}
						, "UpdateSettings?"_= 
						{
							"Names"_= {"--update-settings"}
							,"Default"_= false
							, "Description"_= "Update settings with settings from downloaded app."
						}
						, "RequiredTags?"_= 
						{
							"Names"_= {"--require-tags"}
							, "Type"_= {""}
							, "Description"_= "Require these tags for the version to update to."
						}
					}
					, "Parameters"_=
					{
						"Version?"_=  
						{
							"Default"_= ""
							, "Description"_= "The version to update to.\n"
							"Defaults to the latest version in the same branch.\n"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_UpdateApplication(_Params, false);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-stop"}
					, "Description"_= "Stop the application, keeping any encryption loaded"
					, "Parameters"_=
					{
						"Name"_= 
						{
							"Type"_= ""
							, "Description"_= "Unique name of the application to stop."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_StopApplication(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-restart"}
					, "Description"_= "Restart the application, keeping any encryption loaded"
					, "Parameters"_=
					{
						"Name"_= 
						{
							"Type"_= ""
							, "Description"_= "Unique name of the application to restart."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_RestartApplication(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--application-start"}
					, "Description"_= "Start the application"
					, "Parameters"_=
					{
						"Name"_= 
						{
							"Type"_= ""
							, "Description"_= "Unique name of the application to start."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_StartApplication(_Params);
				}
			)
		;
	}
}
