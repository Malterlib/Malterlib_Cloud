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
		
		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"LogLaunchesToStdErr?"_= 
					{
						"Names"_= {"--log-launches-to-stderr"}
						,"Default"_= false
						, "Description"_= "Log application launch output to stderr"
					}
				}
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
		auto SettingsOption_DistributedApp = "DistributedApp?"_= 
			{
				"Names"_= {"--distributed-app"}
				, "Type"_= true
				, "Description"_= "Expect the app to register as a distributed app. This will cause the AppManager to wait for the app to register before"
					"returning from add, start and update operations."
			}
		;
		auto SettingsOption_UpdateGroup = "UpdateGroup?"_= 
			{
				"Names"_= {"--update-group"}
				, "Type"_= ""
				, "Description"_= "The group to use for coordinating updates with other AppManagers.\n"
			}
		;
		auto SettingsOption_Dependencies = "Dependencies?"_= 
			{
				"Names"_= {"--dependencies"}
				, "Type"_= {""}
				, "Description"_= "The applications this application is dependent on.\n"
			}
		;
		auto SettingsOption_StopOnDependencyFailure = "StopOnDependencyFailure?"_= 
			{
				"Names"_= {"--stop-on-dependency-failure"}
				, "Type"_= true
				, "Description"_= "If this application should automatically stop if one of it's dependencies unexpectedly exits. Defaults to true.\n"
			}
		;

		auto SettingsOption_BackupIncludeWildcards = "BackupIncludeWildcards?"_= 
			{
				"Names"_= {"--backup-include-wildcards"}
				, "Type"_= {""}
				, "Description"_= "The wildcard file searches to include in backups.\n" 
				"Relative to application root. Only file name can have wildcards.\n"
				"Use ^ in the beginning of the file path to create a recursive search.\n"
			}
		;

		auto SettingsOption_BackupExcludeWildcards = "BackupExcludeWildcards?"_= 
			{
				"Names"_= {"--backup-exclude-wildcards"}
				, "Type"_= {""}
				, "Description"_= "Whe wildcard for files to exclude from backup."
				"Relative to application root. Evaluated after include wild cards as a filtering step.\n"
			}
		;

		auto SettingsOption_BackupAddSyncFlagsWildcards = "BackupAddSyncFlagsWildcards?"_= 
			{
				"Names"_= {"--backup-add-sync-flags-wildcards"}
				, "Type"_= {"*"_= {""}}
				, "Description"_= "Specify wildcards mapped to flags to add for files to back up\n" 
				"Relative to application root. Flags:\n"
				"@Indent=20\r"
				"   Append:          Append syncing. Any changes are assumed to be append only.\r"
				"   TransactionLog:  Should be used together with Append. This tells the backup manager to sync writes to disk as quickly as possible.\r"
				"\r"
				"Example: '{\"Logs/^*\": [\"Append\"]}'\n"
			}
		;

		auto SettingsOption_BackupRemoveSyncFlagsWildcards = "BackupRemoveSyncFlagsWildcards?"_= 
			{
				"Names"_= {"--backup-remove-sync-flags-wildcards"}
				, "Type"_= {"*"_= {""}}
				, "Description"_= "Specify wildcards mapped to flags to remove for files to back up\n" 
				"Relative to application root. Evaluated after add sync flags wildcards. Flags:\n"
				"@Indent=20\r"
				"   Append:          Append syncing. Any changes are assumed to be append only.\r"
				"   TransactionLog:  Should be used together with Append. This tells the backup manager to sync writes to disk as quickly as possible.\r"
				"\r"
				"Example: '{\"Logs/*.tmp\": [\"Append\"]}'\n"
			}
		;
		
		auto SettingsOption_BackupNewBackupIntervalHours = "BackupNewBackupIntervalHours?"_= 
			{
				"Names"_= {"--backup-new-backup-interval"}
				, "Type"_= 0.0
				, "Description"_= "Number of hours interval for creating new full backup snapshots. Set to 0 to disable. Defaults to 24 hours\n"
			}
		;

		auto SettingsOption_BackupEnabled = "BackupEnabled?"_= 
			{
				"Names"_= {"--backup-enabled"}
				, "Type"_= true
				, "Description"_= "Enable backups for this application.\n"
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
						, "FromFile?"_= 
						{
							"Names"_= {"--from-file"}
							, "Default"_= false
							, "Description"_= "Install the application from a local file or directory instead of downloading from version manager."
						}
						, "EncryptionStorage?"_= 
						{
							"Names"_= {"--encryption-storage"}
							, "Default"_= ""
							, "Description"_= "Select the file or device that should be the storage for encryption."
						}
						, "EncryptionFileSystem?"_= 
						{
							"Names"_= {"--encryption-file-system"}
							, "Type"_= ""
							, "Description"_= "Select the file system to use for encryption.\n"
							"Currently zfs, xfs and ext4 are supported."
						}
						, "ParentApplication?"_= 
						{
							"Names"_= {"--parent-application"}
							, "Default"_= ""
							, "Description"_= "Put this application as a child to another application, sharing the same root directory.\n"
							"The directory of this application will be: ParentApplicationDir/ApplicationName\n"
						}
						, "Version?"_= 
						{
							"Names"_= {"--version"}
							,"Type"_= "" 
							, "Description"_= "The version to install from version manager.\n"
								"Defaults to the latest version available.\n"
						}
						, "VersionManagerPlatform?"_= 
						{
							"Names"_= {"--platform"}
							, "Type"_= "" 
							, "Description"_= fg_Format
							(
								"Version manager platform used when downloading version. Defaults to the same as this executable: {}"
								, DMalterlibCloudPlatform
							)
						}
						, "ForceOverwrite?"_= 
						{
							"Names"_= {"--force-overwrite"}
							,"Default"_= false 
							, "Description"_= "Force zfs to overwrite storage"
						}
						, "ForceInstall?"_= 
						{
							"Names"_= {"--force-install"}
							,"Default"_= false 
							, "Description"_= "Force application install even if application directory already exists"
						}
						, "SettingsFromVersionInfo?"_= 
						{
							"Names"_= {"--settings-from-version-info"}
							, "Default"_= true 
							, "Description"_= "Get settings from version info of the downloaded application."
						}
						, SettingsOption_Dependencies 
						, SettingsOption_StopOnDependencyFailure
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
						, SettingsOption_DistributedApp
						, SettingsOption_BackupEnabled 
						, SettingsOption_BackupIncludeWildcards 
						, SettingsOption_BackupExcludeWildcards 
						, SettingsOption_BackupAddSyncFlagsWildcards 
						, SettingsOption_BackupRemoveSyncFlagsWildcards 
						, SettingsOption_BackupNewBackupIntervalHours 
						, "AutoUpdateTags?"_= 
						{
							"Names"_= {"--auto-update-tags"}
							, "Default"_= false
							, "Type"_= COneOfType{CEJSON{""}, COneOf{false}}
							, "Description"_= "Auto update the application when new versions become available that has all these these tags."
						}
						, "AutoUpdateBranches?"_= 
						{
							"Names"_= {"--auto-update-branches"}
							, "Default"_= _[_]
							, "Type"_= {""} 
							, "Description"_= "Auto update the application only for versions from these branches.\n"
							"Leave empty to allow any branch.\n"
							"Branches can be matched with wildcards.\n"
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
						, "UpdateScript_OnError?"_= 
						{
							"Names"_= {"--update-script-on-error"}
							, "Default"_= ""
							, "Description"_= "Set a script to run when an error occurs during the update process.\n"
						}
						, "SelfUpdateSource?"_= 
						{
							"Names"_= {"--self-update-source"}
							, "Default"_= false
							, "Description"_= "Set this application as a source for self-updating the app manager.\n"
						}
						, SettingsOption_UpdateGroup
					}
					, "Parameters"_=
					{
						"Package"_=  
						{
							"Type"_= COneOfType{COneOf{nullptr}, ""}
							, "Description"_= "The files needed to run the application.\n"
							"Can be a version manager application name, a directory, a tar.gz file or null. Will look for version manager applications"
							" by default. Specify --from-file to look for the package on disk. Specifying null will add a application with no package. Useful for example when you want to have an encrypted container shared between several applications."
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
						, SettingsOption_Dependencies 
						, SettingsOption_StopOnDependencyFailure
						, SettingsOption_Executable
						, "ExecutableParameters?"_= 
						{
							"Names"_= {"--executable-parameters"}
							, "Type"_= {""} 
							, "Description"_= "Run the application with these executable parameters."
						}				
						, SettingsOption_RunAsUser
						, SettingsOption_RunAsGroup
						, SettingsOption_DistributedApp
						, SettingsOption_BackupEnabled 
						, SettingsOption_BackupIncludeWildcards 
						, SettingsOption_BackupExcludeWildcards 
						, SettingsOption_BackupAddSyncFlagsWildcards 
						, SettingsOption_BackupRemoveSyncFlagsWildcards 
						, SettingsOption_BackupNewBackupIntervalHours 
						, "AutoUpdateTags?"_= 
						{
							"Names"_= {"--auto-update-tags"}
							, "Type"_= COneOfType{CEJSON{""}, COneOf{false}} 
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
							, "Type"_= "" 
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
						, "UpdateScript_OnError?"_= 
						{
							"Names"_= {"--update-script-on-error"}
							, "Type"_= ""
							, "Description"_= "Set a script to run when an error occurs during the update process.\n"
						}
						, "SelfUpdateSource?"_= 
						{
							"Names"_= {"--self-update-source"}
							, "Type"_= false
							, "Description"_= "Set this application as a source for self-updating the app manager.\n"
						}
						, SettingsOption_UpdateGroup
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
						, "Name?"_= 
						{
							"Names"_= {"--name"}
							, "Default"_= "" 
							, "Description"_= "Unique name of the application to list. Leave empty to list all applications." 
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
					return fp_CommandLine_UpdateApplication(_Params);
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
						, "VersionManagerPlatform?"_= 
						{
							"Names"_= {"--platform"}
							, "Type"_= "" 
							, "Description"_= "Version manager platform used when downloading version. Defaults to the same as last installed version."
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
					return fp_CommandLine_UpdateApplication(_Params);
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
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--remove-known-host"}
					, "Description"_= "Remove known host for group and application on this AppManager and any connected remote AppManagers"
					, "Options"_= 
					{
						"Application"_= 
						{
							"Names"_= {"--application"}
							,"Default"_= ""
							, "Description"_= "The version manager application to to remove the host on. Leave empt to remove from all applications."
						}
						, "Group"_= 
						{
							"Names"_= {"--group"}
							,"Default"_= ""
							, "Description"_= "The update group to to remove the host on. Leave empt to remove from all groups."
						}
					}
					, "Parameters"_=
					{
						"HostID"_=  
						{
							"Type"_= ""
							, "Description"_= "The host ID to remove."
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_UpdateApplication(_Params);
				}
			)
		;
		DefaultSection.f_RegisterCommand
			(
				{
					"Names"_= {"--cancel-all-updates"}
					, "Description"_= "Cancel all running pending updates"
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_CancelAllUpdates(_Params);
				}
			)
		;
	}
}
