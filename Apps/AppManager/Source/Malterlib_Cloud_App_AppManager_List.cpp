// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetInstalled() -> TCContinuation<TCMap<CStr, CApplicationInfo>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		
		Permissions["//Command//"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationEnum"}};

		for (auto &pApplication : m_pThis->mp_Applications)
		{
			auto &Application = *pApplication;

			Permissions[Application.m_Name]
				= {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", Application.m_Name)}.f_Description("Access application {} in AppManager"_f << Application.m_Name)}
			;
		}

		TCContinuation<TCMap<CStr, CApplicationInfo>> Continuation;
		pThis->mp_Permissions.f_HasPermissions("Enumerate Apps in AppManager", Permissions)
			> Continuation / [Continuation, Auditor, pThis = m_pThis](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				if (!_HasPermissions["//Command//"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Application enum)"));

				TCMap<CStr, CApplicationInfo> OutputApplications;
				for (auto &pApplication : pThis->mp_Applications)
				{
					auto &Application = *pApplication;

					auto pHasPermission = _HasPermissions.f_FindEqual(Application.m_Name);
					if (!pHasPermission || !*pHasPermission)
						continue;

					auto &OutApplication = OutputApplications[Application.m_Name];
					auto &Settings = Application.m_Settings;

					OutApplication.m_Status = Application.m_LaunchState;
					OutApplication.m_EncryptionStorage = Settings.m_EncryptionStorage;
					OutApplication.m_EncryptionFileSystem = Settings.m_EncryptionFileSystem;
					OutApplication.m_ParentApplication = Settings.m_ParentApplication;
					OutApplication.m_Version = Application.m_LastInstalledVersion;
					OutApplication.m_VersionInfo = Application.m_LastInstalledVersionInfo;
					OutApplication.m_VersionManagerApplication = Settings.m_VersionManagerApplication;
					OutApplication.m_Executable = Settings.m_Executable;
					OutApplication.m_Parameters = Settings.m_ExecutableParameters;
					OutApplication.m_RunAsUser = Settings.m_RunAsUser;
					OutApplication.m_RunAsGroup = Settings.m_RunAsGroup;
					OutApplication.m_bRunAsUserHasShell = Settings.m_bRunAsUserHasShell;
					OutApplication.m_Backup_IncludeWildcards = Settings.m_Backup_IncludeWildcards;
					OutApplication.m_Backup_ExcludeWildcards = Settings.m_Backup_ExcludeWildcards;
					OutApplication.m_Backup_AddSyncFlagsWildcards = Settings.m_Backup_AddSyncFlagsWildcards;
					OutApplication.m_Backup_RemoveSyncFlagsWildcards = Settings.m_Backup_RemoveSyncFlagsWildcards;
					OutApplication.m_Backup_NewBackupInterval = Settings.m_Backup_NewBackupInterval;
					OutApplication.m_AutoUpdateTags = Settings.m_AutoUpdateTags;
					OutApplication.m_AutoUpdateBranches = Settings.m_AutoUpdateBranches;
					OutApplication.m_UpdateScriptPreUpdate = Settings.m_UpdateScripts.m_PreUpdate;
					OutApplication.m_UpdateScriptPostUpdate = Settings.m_UpdateScripts.m_PostUpdate;
					OutApplication.m_UpdateScriptPostLaunch = Settings.m_UpdateScripts.m_PostLaunch;
					OutApplication.m_UpdateScriptOnError = Settings.m_UpdateScripts.m_OnError;
					OutApplication.m_bSelfUpdateSource = Settings.m_bSelfUpdateSource;
					OutApplication.m_UpdateGroup = Settings.m_UpdateGroup;
					OutApplication.m_bDistributedApp = Settings.m_bDistributedApp;
					OutApplication.m_Dependencies = Settings.m_Dependencies;
					OutApplication.m_bStopOnDependencyFailure = Settings.m_bStopOnDependencyFailure;
					OutApplication.m_bBackupEnabled = Settings.m_bBackupEnabled;
				}
				Continuation.f_SetResult(fg_Move(OutputApplications));
				Auditor.f_Info("Enum applications");
			}
		;

		return Continuation;
	}
	
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetAvailableVersions(CStr const &_Application) -> TCContinuation<CVersionsAvailableForUpdate> 
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		
		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["//Command//"] = {{"AppManager/CommandAll", "AppManager/Command/VersionEnum"}};

		for (auto &Application : m_pThis->mp_VersionManagerApplications)
		{
			auto const &Name = Application.f_GetApplicationName();
			if (!_Application.f_IsEmpty() && Name != _Application)
				continue;

			Permissions[Name] = {CPermissionQuery{"AppManager/VersionAppAll", "AppManager/VersionApp/{}"_f << Name}.f_Description("Get versions of application {} in AppManager"_f << Name)};
		}

		TCContinuation<CVersionsAvailableForUpdate> Continuation;
		pThis->mp_Permissions.f_HasPermissions("Enumerate versions in AppManager", Permissions)
			> Continuation / [Continuation, Auditor, pThis = m_pThis, _Application](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				if (!_HasPermissions["//Command//"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Versions available for update)"));

				TCMap<CStr, TCVector<CApplicationVersion>> Versions;

				for (auto &Application : pThis->mp_VersionManagerApplications)
				{
					auto const &Name = Application.f_GetApplicationName();
					if (!_Application.f_IsEmpty() && Name != _Application)
						continue;

					auto pHasPermission = _HasPermissions.f_FindEqual(Name);
					if (!pHasPermission || !*pHasPermission)
						continue;

					auto &OutVersions = Versions[Name];
					for (auto &Version : Application.m_VersionsByTime)
					{
						auto &OutVersion = OutVersions.f_Insert();
						OutVersion.m_VersionID = Version.f_GetVersionID();
						OutVersion.m_VersionInfo = Version.m_VersionInfo;
					}
				}

				Auditor.f_Info("Enum versions available for update");
				Continuation.f_SetResult(fg_Move(Versions));
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_EnumApplications(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr Name = _Params["Name"].f_String();
		TCContinuation<uint32> Continuation;
		mp_AppManagerInterface.m_pActor->f_GetInstalled() 
			> Continuation / [=](TCMap<CStr, CAppManagerInterface::CApplicationInfo> &&_ApplicationInfo)
			{
				for (auto &Application : _ApplicationInfo)
				{
					auto &ApplicationName = _ApplicationInfo.fs_GetKey(Application);
					if (!Name.f_IsEmpty() && ApplicationName != Name)
						continue;

					CStr ApplicationInfo;
					ApplicationInfo += "{}{\n}"_f << ApplicationName;
					if (bVerbose)
					{
						ApplicationInfo += "                       Status: {}{\n}{\n}"_f << Application.m_Status;
						ApplicationInfo += "           Encryption storage: {}{\n}"_f << Application.m_EncryptionStorage;
						ApplicationInfo += "       Encryption file system: {}{\n}"_f << Application.m_EncryptionFileSystem;
						ApplicationInfo += "           Parent application: {}{\n}"_f << Application.m_ParentApplication;
						ApplicationInfo += "                 Dependencies: {vs}{\n}"_f << Application.m_Dependencies;
						ApplicationInfo += "   Stop on dependency failure: {}{\n}"_f << (Application.m_bStopOnDependencyFailure ? "true" : "false");
						ApplicationInfo += "           Self update source: {}{\n}{\n}"_f << (Application.m_bSelfUpdateSource ? "true" : "false");
						ApplicationInfo += "                   Executable: {}{\n}"_f << Application.m_Executable;
						ApplicationInfo += "                   Parameters: {vs,vb}{\n}"_f << Application.m_Parameters;
						ApplicationInfo += "                  Run as user: {}{\n}"_f << Application.m_RunAsUser;
						ApplicationInfo += "                 Run as group: {}{\n}"_f << Application.m_RunAsGroup;
						ApplicationInfo += "        Run as user has shell: {}{\n}"_f << (Application.m_bRunAsUserHasShell ? "true" : "false");
						ApplicationInfo += "              Distributed app: {}{\n}{\n}"_f << (Application.m_bDistributedApp ? "true" : "false");
						
						ApplicationInfo += "               Backup enabled: {}{\n}"_f << (Application.m_bBackupEnabled ? "true" : "false");
						ApplicationInfo += "     Backup include wildcards: {vs}{\n}"_f << Application.m_Backup_IncludeWildcards;
						ApplicationInfo += "     Backup exclude wildcards: {vs}{\n}"_f << Application.m_Backup_ExcludeWildcards;
						ApplicationInfo += "        Backup add sync flags: {vs}{\n}"_f << Application.m_Backup_AddSyncFlagsWildcards;
						ApplicationInfo += "     Backup remove sync flags: {vs}{\n}"_f << Application.m_Backup_RemoveSyncFlagsWildcards;
						ApplicationInfo += "        Backup interval hours: {}{\n}{\n}"_f << CTimeSpanConvert(Application.m_Backup_NewBackupInterval).f_GetHoursFloat();

						ApplicationInfo += "                  Auto update: {}{\n}"_f << (!Application.m_AutoUpdateTags.f_IsEmpty() ? "true" : "false");
						ApplicationInfo += "             Auto update tags: {vs}{\n}"_f << Application.m_AutoUpdateTags;
						ApplicationInfo += "         Auto update branches: {vs}{\n}{\n}"_f << Application.m_AutoUpdateBranches;

						ApplicationInfo += "            Pre update script: {}{\n}"_f << Application.m_UpdateScriptPreUpdate;
						ApplicationInfo += "           Post update script: {}{\n}"_f << Application.m_UpdateScriptPostUpdate;
						ApplicationInfo += "    Post launch update script: {}{\n}"_f << Application.m_UpdateScriptPostLaunch;
						ApplicationInfo += "       On error update script: {}{\n}{\n}"_f << Application.m_UpdateScriptOnError;

						ApplicationInfo += "     Version application name: {}{\n}"_f << Application.m_VersionManagerApplication;
						ApplicationInfo += "                 Update group: {}{\n}"_f << Application.m_UpdateGroup;
						ApplicationInfo += "                      Version: {}{\n}"_f << Application.m_Version;
						ApplicationInfo += "                 Version time: {}{\n}"_f << Application.m_VersionInfo.m_Time.f_ToLocal();
						ApplicationInfo += "               Version config: {}{\n}"_f << Application.m_VersionInfo.m_Configuration;
						ApplicationInfo += "                 Version size: {}{\n}"_f << Application.m_VersionInfo.m_nBytes;
						ApplicationInfo += "                Version files: {}{\n}"_f << Application.m_VersionInfo.m_nFiles;

						if (Application.m_VersionInfo.m_ExtraInfo.f_IsObject())
						{
							CStr InfoString = Application.m_VersionInfo.m_ExtraInfo.f_ToString("    ");
							CStr FirstLine = fg_GetStrLineSep(InfoString);
							ApplicationInfo += "                Version extra: {}{\n}"_f << FirstLine;
							while (!InfoString.f_IsEmpty())
								ApplicationInfo += "                               {}{\n}"_f << fg_GetStrLineSep(InfoString);
						}
					}
					*_pCommandLine += ApplicationInfo;
				}
				Continuation.f_SetResult(0);
			}
		;
		return Continuation;
	}
	
	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_ListAvailableVersions(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
	
		TCContinuation<uint32> Continuation;
		mp_AppManagerInterface.m_pActor->f_GetAvailableVersions(_Params["Application"].f_String()) 
			> Continuation / [=](CAppManagerInterface::CVersionsAvailableForUpdate &&_Results)
			{
				smint LongestApplication = fg_StrLen("Application");
				smint LongestVersion = fg_StrLen("Version");
				smint LongestPlatform = fg_StrLen("Platform");
				smint LongestConfig = fg_StrLen("Config");
				smint LongestTime = fg_StrLen("Time");
				smint LongestSize = fg_StrLen("Size");
				smint LongestFiles = fg_StrLen("Files");
				smint LongestTags = fg_StrLen("Tags");
				smint LongestRetrySequence = fg_StrLen("RetrySequence");
				for (auto &Versions : _Results)
				{
					LongestApplication = fg_Max(LongestApplication, _Results.fs_GetKey(Versions).f_GetLen());
					for (auto &Version : Versions)
					{
						LongestVersion = fg_Max(LongestVersion, fg_Format("{}", Version.m_VersionID.m_VersionID).f_GetLen());
						LongestPlatform = fg_Max(LongestPlatform, fg_Format("{}", Version.m_VersionID.m_Platform).f_GetLen());
						LongestConfig = fg_Max(LongestConfig, Version.m_VersionInfo.m_Configuration.f_GetLen());
						LongestTime = fg_Max(LongestTime, fg_Format("{tc6}", Version.m_VersionInfo.m_Time.f_ToLocal()).f_GetLen());
						LongestSize = fg_Max(LongestSize, fg_Format("{ns }", Version.m_VersionInfo.m_nBytes).f_GetLen());
						LongestFiles = fg_Max(LongestFiles, fg_Format("{}", Version.m_VersionInfo.m_nFiles).f_GetLen());
						LongestTags = fg_Max(LongestTags, fg_Format("{vs,vb}", Version.m_VersionInfo.m_Tags).f_GetLen());
						LongestRetrySequence = fg_Max(LongestFiles, fg_Format("{}", Version.m_VersionInfo.m_RetrySequence).f_GetLen());
					}
				}
				
				auto fOutputLine = [&]
					(
						auto const &_Application
						, auto const &_Version
						, auto const &_Platform
						, auto const &_Config
						, auto const &_Time
						, auto const &_Size
						, auto const &_Files
						, auto const &_Tags
						, auto const &_RetrySequence
					)
					{
						*_pCommandLine += "{sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*}   {sj*}   {sj*,a-}   {sj*,a-}\n"_f
							<< _Application
							<< LongestApplication
							<< _Version
							<< LongestVersion
							<< _Platform
							<< LongestPlatform
							<< _Config
							<< LongestConfig
							<< _Time
							<< LongestTime
							<< _Size
							<< LongestSize
							<< _Files
							<< LongestFiles
							<< _Tags
							<< LongestTags
							<< _RetrySequence
							<< LongestRetrySequence
						;
					}
				;
				
				fOutputLine("Application", "Version", "Platform", "Config", "Time", "Size", "Files", "Tags", "RetrySequence");
				
				for (auto &Versions : _Results)
				{
					auto &ApplicationName = _Results.fs_GetKey(Versions);
					for (auto &Version : Versions)
					{
						fOutputLine
							(
								ApplicationName
								, Version.m_VersionID.m_VersionID
								, Version.m_VersionID.m_Platform
								, Version.m_VersionInfo.m_Configuration
								, fg_Format("{tc6}", Version.m_VersionInfo.m_Time.f_ToLocal())
								, fg_Format("{ns }", Version.m_VersionInfo.m_nBytes)
								, fg_Format("{}", Version.m_VersionInfo.m_nFiles)
								, fg_Format("{vs,vb}", Version.m_VersionInfo.m_Tags)
								, fg_Format("{}", Version.m_VersionInfo.m_RetrySequence)
							)
						;
						if (bVerbose && Version.m_VersionInfo.m_ExtraInfo.f_IsObject() && Version.m_VersionInfo.m_ExtraInfo.f_Object().f_OrderedIterator())
						{
							CStr JSONString = Version.m_VersionInfo.m_ExtraInfo.f_ToString("    ");
							while (!JSONString.f_IsEmpty())
							{
								CStr Line = fg_GetStrLineSep(JSONString);
								*_pCommandLine += "{}\n"_f << Line;
							}
						}
					}
				}
				
				Continuation.f_SetResult(0);
			}
		;
		
		return Continuation;
	}
}
