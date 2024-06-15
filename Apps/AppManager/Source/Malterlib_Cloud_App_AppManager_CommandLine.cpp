// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/CommandLine/TableRenderer>

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
					"LogLaunchesToStdErr?"_o=
					{
						"Names"_o= {"--log-launches-to-stderr"}
						,"Default"_o= false
						, "Description"_o= "Log application launch output to stderr."
					}
				}
			)
		;

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"AutoUpdateDelay?"_o=
					{
						"Names"_o= {"--auto-update-delay"}
						,"Default"_o= fg_GetSys()->f_GetEnvironmentVariable
						(
							"MalterlibAppManagerAutoUpdateDelay"
							, CStr::fs_ToStr(fp64(mc_DefaultAutoUpdateDelay))
						).f_ToFloat(fp64(mc_DefaultAutoUpdateDelay))
						, "Description"_o= "Delay wait when receiving new versions to wait for other version managers to also send their versions."
					}
				}
			)
		;

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"HostMonitorInterval?"_o=
					{
						"Names"_o= {"--host-monitor-interval"}
						,"Type"_o= 0.0
						, "Description"_o= "Override host monitor interval."
					}
				}
			)
		;

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"HostMonitorPatchInterval?"_o=
					{
						"Names"_o= {"--host-monitor-patch-interval"}
						,"Type"_o= 0.0
						, "Description"_o= "Override host monitor patch interval."
					}
				}
			)
		;

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"EnableApplicationStatusSensors?"_o=
					{
						"Names"_o= {"--application-status-sensors"}
						,"Default"_o= true
						, "Description"_o= "Enable application status sensors."
					}
				}
			)
		;

		o_CommandLine.f_RegisterGlobalOptions
			(
				{
					"EnableEncryptionStatusSensors?"_o=
					{
						"Names"_o= {"--encryption-status-sensors"}
						,"Default"_o= true
						, "Description"_o= "Enable application encryption sensors."
					}
				}
			)
		;

		auto ApplicationManagement = o_CommandLine.f_AddSection("Application Management", "Commands to manage AppManager applications.");

		auto SettingsOption_Executable = "Executable?"_o=
			{
				"Names"_o= {"--executable"}
				,"Type"_o= ""
				, "Description"_o= "Start this executable when running application.\n"
					"Can contain sub-path.\n"
			}
		;
		auto SettingsOption_RunAsUser = "RunAsUser?"_o=
			{
				"Names"_o= {"--run-as-user"}
				, "Type"_o= ""
				, "Description"_o= "Run the application as this user."
			}
		;
#ifdef DPlatformFamily_Windows
		auto SettingsOption_RunAsUserPassword = "RunAsUserPassword?"_o=
			{
				"Names"_o= {"--run-as-user-password"}
				, "Type"_o= ""
				, "Description"_o= "Set the password for the run as user.\n"
				"This password is automatically set when a user is created. If you change it you can use this function to change the password used to launch the application."
			}
		;
#endif
		auto SettingsOption_RunAsGroup = "RunAsGroup?"_o=
			{
				"Names"_o= {"--run-as-group"}
				, "Type"_o= ""
				, "Description"_o= "Run the application as this group."
			}
		;
		auto SettingsOption_RunAsUserHasShell = "RunAsUserHasShell?"_o=
			{
				"Names"_o= {"--run-as-user-has-shell"}
				, "Type"_o= ""
				, "Description"_o= "The run as user gets created with shell access."
			}
		;
		auto SettingsOption_LaunchInProcess = "LaunchInProcess?"_o=
			{
				"Names"_o= {"--launch-in-process"}
				, "Type"_o= true
				, "Description"_o= "Launch the application in the process of the AppManager.\n"
				"This is only useful when you have populated the in app registry. See fg_AppManager_RegisterInProcessFactory."
				" The default AppManager executable does not include any bundled applications inside the executable."
			}
		;
		auto SettingsOption_DistributedApp = "DistributedApp?"_o=
			{
				"Names"_o= {"--distributed-app"}
				, "Type"_o= true
				, "Description"_o= "Expect the app to register as a distributed app. This will cause the AppManager to wait for the app to register before."
					"returning from add, start and update operations."
			}
		;
		auto SettingsOption_UpdateGroup = "UpdateGroup?"_o=
			{
				"Names"_o= {"--update-group"}
				, "Type"_o= ""
				, "Description"_o= "The group to use for coordinating updates with other AppManagers.\n"
			}
		;
		auto SettingsOption_Dependencies = "Dependencies?"_o=
			{
				"Names"_o= {"--dependencies"}
				, "Type"_o= {""}
				, "Description"_o= "The applications this application is dependent on.\n"
			}
		;
		auto SettingsOption_StopOnDependencyFailure = "StopOnDependencyFailure?"_o=
			{
				"Names"_o= {"--stop-on-dependency-failure"}
				, "Type"_o= true
				, "Description"_o= "If this application should automatically stop if one of it's dependencies unexpectedly exits. Defaults to true.\n"
			}
		;

		auto SettingsOption_BackupIncludeWildcards = "BackupIncludeWildcards?"_o=
			{
				"Names"_o= {"--backup-include-wildcards"}
				, "Type"_o= {"*"_o= {COneOfType{CEJSONOrdered{""}, COneOf{nullptr}}}}
				, "Description"_o= "The wildcard file searches to include in backups.\n"
				"Relative to application root. Only file name can have wildcards.\n"
				"Use ^ in the beginning of the file path to create a recursive search.\n"
				"Example: '{\"Logs/^*\": null, \"Files/^*\": \"Files2\"}'\n"
			}
		;

		auto SettingsOption_BackupExcludeWildcards = "BackupExcludeWildcards?"_o=
			{
				"Names"_o= {"--backup-exclude-wildcards"}
				, "Type"_o= {""}
				, "Description"_o= "Whe wildcard for files to exclude from backup."
				"Relative to application root. Evaluated after include wild cards as a filtering step.\n"
			}
		;

		auto SettingsOption_BackupAddSyncFlagsWildcards = "BackupAddSyncFlagsWildcards?"_o=
			{
				"Names"_o= {"--backup-add-sync-flags-wildcards"}
				, "Type"_o= {"*"_o= {""}}
				, "Description"_o= "Specify wildcards mapped to flags to add for files to back up.\n"
				"Relative to application root. Flags:\n"
				"@Indent=20\r"
				"   Append:          Append syncing. Any changes are assumed to be append only.\r"
				"   TransactionLog:  Should be used together with Append. This tells the backup manager to sync writes to disk as quickly as possible.\r"
				"\r"
				"Example: '{\"Logs/*\": [\"Append\"]}'\n"
			}
		;

		auto SettingsOption_BackupRemoveSyncFlagsWildcards = "BackupRemoveSyncFlagsWildcards?"_o=
			{
				"Names"_o= {"--backup-remove-sync-flags-wildcards"}
				, "Type"_o= {"*"_o= {""}}
				, "Description"_o= "Specify wildcards mapped to flags to remove for files to back up.\n"
				"Relative to application root. Evaluated after add sync flags wildcards. Flags:\n"
				"@Indent=20\r"
				"   Append:          Append syncing. Any changes are assumed to be append only.\r"
				"   TransactionLog:  Should be used together with Append. This tells the backup manager to sync writes to disk as quickly as possible.\r"
				"\r"
				"Example: '{\"Logs/*.tmp\": [\"Append\"]}'\n"
			}
		;
		
		auto SettingsOption_BackupNewBackupIntervalHours = "BackupNewBackupIntervalHours?"_o=
			{
				"Names"_o= {"--backup-new-backup-interval"}
				, "Type"_o= 0.0
				, "Description"_o= "Number of hours interval for creating new full backup snapshots. Set to 0 to disable. Defaults to 24 hours.\n"
			}
		;

		auto SettingsOption_BackupEnabled = "BackupEnabled?"_o=
			{
				"Names"_o= {"--backup-enabled"}
				, "Type"_o= true
				, "Description"_o= "Enable backups for this application.\n"
			}
		;

		auto AddOption_AutoUpdate = "AutoUpdate?"_o=
			{
				"Names"_o= {"--auto-update"}
				, "Default"_o= true
				, "Description"_o= "Automatically update the application."
			}
		;

		auto AddOption_UpdateTags = "UpdateTags?"_o=
			{
				"Names"_o= {"--update-tags"}
				, "Default"_o= _[_]
				, "Type"_o= CEJSONOrdered{""}
				, "Description"_o= "When updating the application require these tags on the version to update to."
			}
		;
		auto AddOption_UpdateBranches = "UpdateBranches?"_o=
			{
				"Names"_o= {"--update-branches"}
				, "Default"_o= _[_]
				, "Type"_o= {""}
				, "Description"_o= "Update the application only for versions from these branches.\n"
				"Leave empty to allow any branch.\n"
				"Branches can be matched with wildcards.\n"
			}
		;
		auto AddOption_UpdateScriptPreUpdate = "UpdateScript_PreUpdate?"_o=
			{
				"Names"_o= {"--update-script-pre-update"}
				, "Default"_o= ""
				, "Description"_o= "Set a script to run pre update.\n"
			}
		;
		auto AddOption_UpdateScriptPostUpdate = "UpdateScript_PostUpdate?"_o=
			{
				"Names"_o= {"--update-script-post-update"}
				, "Default"_o= ""
				, "Description"_o= "Set a script to run post update.\n"
			}
		;
		auto AddOption_UpdateScriptPostLaunch = "UpdateScript_PostLaunch?"_o=
			{
				"Names"_o= {"--update-script-post-launch"}
				, "Default"_o= ""
				, "Description"_o= "Set a script to run post launch.\n"
			}
		;
		auto AddOption_UpdateScriptOnError = "UpdateScript_OnError?"_o=
			{
				"Names"_o= {"--update-script-on-error"}
				, "Default"_o= ""
				, "Description"_o= "Set a script to run when an error occurs during the update process.\n"
			}
		;

		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-add"}
					, "Description"_o=
						"Adds an application.\n"
						"By default the application will run as root."
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= {"--name"}
							,"Default"_o= ""
							, "Description"_o= "Uniquely name the application.\n"
							"If left empty the name defaults to the package name when installing from a Versionanager, "
							"otherwise if --from-file or a null package is specified specifying a name is required."
						}
						, "FromFile?"_o=
						{
							"Names"_o= {"--from-file"}
							, "Default"_o= false
							, "Description"_o= "Install the application from a local file or directory instead of downloading from version manager."
						}
						, "EncryptionStorage?"_o=
						{
							"Names"_o= {"--encryption-storage"}
							, "Default"_o= ""
							, "Description"_o= "Select the file or device that should be the storage for encryption."
						}
						, "EncryptionFileSystem?"_o=
						{
							"Names"_o= {"--encryption-file-system"}
							, "Type"_o= ""
							, "Description"_o= "Select the file system to use for encryption.\n"
							"Currently zfs, xfs and ext4 are supported."
						}
						, "ParentApplication?"_o=
						{
							"Names"_o= {"--parent-application"}
							, "Default"_o= ""
							, "Description"_o= "Put this application as a child to another application, sharing the same root directory.\n"
							"The directory of this application will be: ParentApplicationDir/ApplicationName\n"
						}
						, "Version?"_o=
						{
							"Names"_o= {"--version"}
							,"Type"_o= ""
							, "Description"_o= "The version to install from version manager.\n"
								"Defaults to the latest version available.\n"
						}
						, "VersionManagerPlatform?"_o=
						{
							"Names"_o= {"--platform"}
							, "Type"_o= ""
							, "Description"_o= fg_Format
							(
								"Version manager platform used when downloading version. Defaults to the same as this executable: {}"
								, DMalterlibCloudPlatform
							)
						}
						, "ForceOverwrite?"_o=
						{
							"Names"_o= {"--force-overwrite"}
							,"Default"_o= false
							, "Description"_o= "Force zfs to overwrite storage."
						}
						, "ForceInstall?"_o=
						{
							"Names"_o= {"--force-install"}
							,"Default"_o= false
							, "Description"_o= "Force application install even if application directory already exists."
						}
						, "SettingsFromVersionInfo?"_o=
						{
							"Names"_o= {"--settings-from-version-info"}
							, "Default"_o= true
							, "Description"_o= "Get settings from version info of the downloaded application."
						}
						, SettingsOption_Dependencies 
						, SettingsOption_StopOnDependencyFailure
						, SettingsOption_Executable
						, "ExecutableParameters?"_o=
						{
							"Names"_o= {"--executable-parameters"}
							, "Default"_o= {"--daemon-run-standalone"}
							, "Type"_o= {""}
							, "Description"_o= "Run the application with these executable parameters."
						}
						, SettingsOption_RunAsUser
#ifdef DPlatformFamily_Windows
						, SettingsOption_RunAsUserPassword
#endif
						, SettingsOption_RunAsGroup
						, SettingsOption_RunAsUserHasShell
						, SettingsOption_LaunchInProcess
						, SettingsOption_DistributedApp
						, SettingsOption_BackupEnabled 
						, SettingsOption_BackupIncludeWildcards 
						, SettingsOption_BackupExcludeWildcards 
						, SettingsOption_BackupAddSyncFlagsWildcards 
						, SettingsOption_BackupRemoveSyncFlagsWildcards 
						, SettingsOption_BackupNewBackupIntervalHours
						, AddOption_AutoUpdate
						, AddOption_UpdateTags
						, AddOption_UpdateBranches
						, AddOption_UpdateScriptPreUpdate
						, AddOption_UpdateScriptPostUpdate
						, AddOption_UpdateScriptPostLaunch
						, AddOption_UpdateScriptOnError
						, "SelfUpdateSource?"_o=
						{
							"Names"_o= {"--self-update-source"}
							, "Default"_o= false
							, "Description"_o= "Set this application as a source for self-updating the app manager.\n"
						}
						, SettingsOption_UpdateGroup
					}
					, "Parameters"_o=
					{
						"Package"_o=
						{
							"Type"_o= COneOfType{COneOf{nullptr}, ""}
							, "Description"_o= "The files needed to run the application.\n"
							"Can be a version manager application name, a directory, a tar.gz file or null. Will look for version manager applications"
							" by default. Specify --from-file to look for the package on disk. Specifying null will add a application with no package. "
							"Useful for example when you want to have an encrypted container shared between several applications."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_AddApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-enable-self-update"}
					, "Description"_o=
						"Adds an AppManager application that is used for self updating this AppManager.\n"
						"This is a shortcut for doing --application-add with --self-update-source specified.\n"
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							,"Default"_o= "SelfUpdate"
						}
						, "FromFile?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= false
						}
						, "EncryptionStorage?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= ""
						}
						, "ParentApplication?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= ""
						}
						, "ForceOverwrite?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							,"Default"_o= false
						}
						, "ForceInstall?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							,"Default"_o= false
						}
						, "SettingsFromVersionInfo?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= true
						}
						, "ExecutableParameters?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= _[_]
						}
						, "SelfUpdateSource?"_o=
						{
							"Names"_o= _[_]
							, "Hidden"_o= true
							, "Description"_o= "Hidden"
							, "Default"_o= true
						}
						, AddOption_AutoUpdate
						, AddOption_UpdateTags
						, AddOption_UpdateBranches
						, AddOption_UpdateScriptPreUpdate
						, AddOption_UpdateScriptPostUpdate
						, AddOption_UpdateScriptPostLaunch
						, AddOption_UpdateScriptOnError
						, SettingsOption_UpdateGroup
					}
					, "Parameters"_o=
					{
						"Package?"_o=
						{
							"Default"_o= "AppManager"
							, "Description"_o= "Package AppManager defaulted for self update."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_AddApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-change-settings"}
					, "Description"_o= "Change settings for application.\n"
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= {"--name"}
							,"Type"_o= ""
							, "Description"_o= "Unique name of the application to change settings for."
						}
						, SettingsOption_Dependencies 
						, SettingsOption_StopOnDependencyFailure
						, SettingsOption_Executable
						, "ExecutableParameters?"_o=
						{
							"Names"_o= {"--executable-parameters"}
							, "Type"_o= {""}
							, "Description"_o= "Run the application with these executable parameters."
						}				
						, SettingsOption_RunAsUser
#ifdef DPlatformFamily_Windows
						, SettingsOption_RunAsUserPassword
#endif
						, SettingsOption_RunAsGroup
						, SettingsOption_RunAsUserHasShell
						, SettingsOption_LaunchInProcess
						, SettingsOption_DistributedApp
						, SettingsOption_BackupEnabled 
						, SettingsOption_BackupIncludeWildcards 
						, SettingsOption_BackupExcludeWildcards 
						, SettingsOption_BackupAddSyncFlagsWildcards 
						, SettingsOption_BackupRemoveSyncFlagsWildcards 
						, SettingsOption_BackupNewBackupIntervalHours
						, "AutoUpdate?"_o=
						{
							"Names"_o= {"--auto-update"}
							, "Type"_o= true
							, "Description"_o= "Automatically update the application."
						}
						, "UpdateTags?"_o=
						{
							"Names"_o= {"--update-tags"}
							, "Type"_o= CEJSONOrdered{""}
							, "Description"_o= "When updating the application require these tags on the version to update to."
						}
						, "UpdateBranches?"_o=
						{
							"Names"_o= {"--update-branches"}
							, "Type"_o= {""}
							, "Description"_o= "Update the application only for versions from these branches.\n"
							"Leave empty to allow any branch\n"
						}
						, "VersionManagerApplication?"_o=
						{
							"Names"_o= {"--version-manager-application"}
							, "Type"_o= ""
							, "Description"_o= "Get updates from the version manager application with this name."
						}				
						, "UpdateFromVersionInfo?"_o=
						{
							"Names"_o= {"--update-from-version-info"}
							, "Default"_o= false
							, "Description"_o= "Update settings from the last installed version manager application info."
						}				
						, "Force?"_o=
						{
							"Names"_o= {"--force"}
							, "Default"_o= false
							, "Description"_o= "Force running the update process even if no settings are changed."
						}				
						, "UpdateScript_PreUpdate?"_o=
						{
							"Names"_o= {"--update-script-pre-update"}
							, "Type"_o= ""
							, "Description"_o= "Set a script to run pre update.\n"
						}
						, "UpdateScript_PostUpdate?"_o=
						{
							"Names"_o= {"--update-script-post-update"}
							, "Type"_o= ""
							, "Description"_o= "Set a script to run post update.\n"
						}
						, "UpdateScript_PostLaunch?"_o=
						{
							"Names"_o= {"--update-script-post-launch"}
							, "Type"_o= ""
							, "Description"_o= "Set a script to run post launch.\n"
						}
						, "UpdateScript_OnError?"_o=
						{
							"Names"_o= {"--update-script-on-error"}
							, "Type"_o= ""
							, "Description"_o= "Set a script to run when an error occurs during the update process.\n"
						}
						, "SelfUpdateSource?"_o=
						{
							"Names"_o= {"--self-update-source"}
							, "Type"_o= false
							, "Description"_o= "Set this application as a source for self-updating the app manager.\n"
						}
						, SettingsOption_UpdateGroup
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_ChangeApplicationSettings, _Params, _pCommandLine);
				}
			)
		;
		
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-list"}
					, "Description"_o= "List applications."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the applications."
						}
						, "ExtraVerbose?"_o=
						{
							"Names"_o= {"--extra-verbose", "-vv"}
							, "Default"_o= false
							, "Description"_o= "Display even more extensive information about the applications."
						}
						, "Name?"_o=
						{
							"Names"_o= {"--name"}
							, "Default"_o= ""
							, "Description"_o= "Unique name of the application to list. Leave empty to list all applications."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_EnumApplications, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-list-versions"}
					, "Description"_o= "List versions available to update to."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Display more extensive information about the versions."
						}
						, "Application?"_o=
						{
							"Names"_o= {"--application"}
							, "Default"_o= ""
							, "Description"_o= "The application to list versions for.\n"
								"Leave empty to list all applications.\n"
						}
						, "ExtraPlatforms?"_o=
						{
							"Names"_o= {"--platforms"}
							, "Type"_o= {""}
							, "Default"_o= _[_]
							, "Description"_o= "Add non-default platforms you want to include in the list.\n"
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_ListAvailableVersions, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-remove"}
					, "Description"_o= "Remove the application."
					, "Parameters"_o=
					{
						"Name"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The name of the application to remove."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_RemoveApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-update-from-file"}
					, "Description"_o= "Update the application package from file."
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= {"--name"}
							,"Type"_o= ""
							, "Description"_o= "Unique name of the application to update."
						}
					}
					, "Parameters"_o=
					{
						"Package"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The files needed to run the application.\n"
							"Can be a directory, or a tar.gz file."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_UpdateApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-update"}
					, "Description"_o= "Update the application package from version manager."
					, "Options"_o=
					{
						"Name"_o=
						{
							"Names"_o= {"--name"}
							,"Type"_o= ""
							, "Description"_o= "Unique name of the application to update."
						}
						, "DryRun?"_o=
						{
							"Names"_o= {"--dry-run"}
							,"Default"_o= false
							, "Description"_o= "Only list action to take, don't actually do the update."
						}
						, "UpdateSettings?"_o=
						{
							"Names"_o= {"--update-settings"}
							,"Default"_o= false
							, "Description"_o= "Update settings with settings from downloaded app."
						}
						, "RequiredTags?"_o=
						{
							"Names"_o= {"--require-tags"}
							, "Type"_o= {""}
							, "Description"_o= "Require these tags for the version to update to."
						}
						, "VersionManagerPlatform?"_o=
						{
							"Names"_o= {"--platform"}
							, "Type"_o= ""
							, "Description"_o= "Version manager platform used when downloading version. Defaults to the same as last installed version."
						}
					}
					, "Parameters"_o=
					{
						"Version?"_o=
						{
							"Default"_o= ""
							, "Description"_o= "The version to update to.\n"
							"Defaults to the latest version in the same branch.\n"
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_UpdateApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-stop"}
					, "Description"_o= "Stop the application, keeping any encryption loaded."
					, "Parameters"_o=
					{
						"Name"_o=
						{
							"Type"_o= ""
							, "Description"_o= "Unique name of the application to stop."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_StopApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-restart"}
					, "Description"_o= "Restart the application, keeping any encryption loaded."
					, "Parameters"_o=
					{
						"Name"_o=
						{
							"Type"_o= ""
							, "Description"_o= "Unique name of the application to restart."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_RestartApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--application-start"}
					, "Description"_o= "Start the application."
					, "Parameters"_o=
					{
						"Name"_o=
						{
							"Type"_o= ""
							, "Description"_o= "Unique name of the application to start."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_StartApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--remove-known-host"}
					, "Description"_o= "Remove known host for group and application on this AppManager and any connected remote AppManagers."
					, "Options"_o=
					{
						"Application"_o=
						{
							"Names"_o= {"--application"}
							,"Default"_o= ""
							, "Description"_o= "The version manager application to to remove the host on. Leave empt to remove from all applications."
						}
						, "Group"_o=
						{
							"Names"_o= {"--group"}
							,"Default"_o= ""
							, "Description"_o= "The update group to to remove the host on. Leave empt to remove from all groups."
						}
					}
					, "Parameters"_o=
					{
						"HostID"_o=
						{
							"Type"_o= ""
							, "Description"_o= "The host ID to remove."
						}
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_UpdateApplication, _Params, _pCommandLine);
				}
			)
		;
		ApplicationManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--cancel-all-updates"}
					, "Description"_o= "Cancel all running pending updates."
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_CancelAllUpdates, _Params, _pCommandLine);
				}
			)
		;

		auto ExternalManagement = o_CommandLine.f_AddSection("External Dependencies", "Commands to manage external dependencies.");
		ExternalManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--version-manager-list"}
					, "Description"_o= "List connected version managers."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Display more extensive information."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_VersionManagerList, _Params, _pCommandLine);
				}
			)
		;
		ExternalManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--cloud-manager-list"}
					, "Description"_o= "List connected cloud managers."
					, "Options"_o=
					{
						"Verbose?"_o=
						{
							"Names"_o= {"--verbose", "-v"}
							, "Default"_o= false
							, "Description"_o= "Display more extensive information."
						}
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_CloudManagerList, _Params, _pCommandLine);
				}
			)
		;
		ExternalManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--stored-update-notifications-list"}
					, "Description"_o= "List stored update notifications."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_StoredUpdateNotificationList, _Params, _pCommandLine);
				}
			)
		;
		ExternalManagement.f_RegisterCommand
			(
				{
					"Names"_o= {"--stored-update-notifications-clear"}
					, "Description"_o= "Clear stored update notifications."
					, "Options"_o=
					{
						CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return g_Future <<= self(&CAppManagerActor::fp_CommandLine_StoredUpdateNotificationClear, _Params, _pCommandLine);
				}
			)
		;

		fp_BuildCommandLine_HostMonitor(o_CommandLine);
	}
}
