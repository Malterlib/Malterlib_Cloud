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
		CStr CallingHostID = fg_GetCallingHostID();
		
		if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationEnum"))
			return Auditor.f_AccessDenied("(Application enum)");

		TCMap<CStr, CApplicationInfo> OutputApplications;
		
		CDistributedAppCommandLineResults Results;
		for (auto &pApplication : m_pThis->mp_Applications)
		{
			auto &Application = *pApplication;
			
			if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/AppAll", fg_Format("AppManager/App/{}", Application.m_Name)))
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
			OutApplication.m_AutoUpdateTags = Settings.m_AutoUpdateTags;
			OutApplication.m_AutoUpdateBranches = Settings.m_AutoUpdateBranches;
			OutApplication.m_UpdateScriptPreUpdate = Settings.m_UpdateScripts.m_PreUpdate;
			OutApplication.m_UpdateScriptPostUpdate = Settings.m_UpdateScripts.m_PostUpdate;
			OutApplication.m_UpdateScriptPostLaunch = Settings.m_UpdateScripts.m_PostLaunch;
			OutApplication.m_UpdateScriptOnError = Settings.m_UpdateScripts.m_OnError;
			OutApplication.m_bSelfUpdateSource = Settings.m_bSelfUpdateSource;
		}
		Auditor.f_Info("Enum applications");
		return fg_Explicit(fg_Move(OutputApplications));
	}
	
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_GetAvailableVersions(CStr const &_Application) -> TCContinuation<CVersionsAvailableForUpdate> 
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		CStr CallingHostID = fg_GetCallingHostID();
		
		if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/VersionEnum"))
			return Auditor.f_AccessDenied("(Versions available for update)");

		TCMap<CStr, TCVector<CApplicationVersion>> Versions;
			
		for (auto &Application : m_pThis->mp_VersionManagerApplications)
		{
			if (!_Application.f_IsEmpty() && Application.f_GetApplicationName() != _Application)
				continue;

			if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", Application.f_GetApplicationName())))
				continue;
			
			auto &OutVersions = Versions[Application.f_GetApplicationName()];
			for (auto &Version : Application.m_VersionsByTime)
			{
				auto &OutVersion = OutVersions.f_Insert();
				OutVersion.m_VersionID = Version.f_GetVersionID();
				OutVersion.m_VersionInfo = Version.m_VersionInfo; 
			}
		}
		
		Auditor.f_Info("Enum versions available for update");
		return fg_Explicit(fg_Move(Versions));
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_EnumApplications(CEJSON const &_Params)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		mp_AppManagerInterface.m_pActor->f_GetInstalled() 
			> Continuation / [bVerbose, Continuation](TCMap<CStr, CAppManagerInterface::CApplicationInfo> &&_ApplicationInfo)
			{
				CDistributedAppCommandLineResults Results;
				for (auto &Application : _ApplicationInfo)
				{
					Results.f_AddStdOut(fg_Format("{}{\n}", _ApplicationInfo.fs_GetKey(Application)));
					if (bVerbose)
					{
						Results.f_AddStdOut(fg_Format("                       Status: {}{\n}{\n}", Application.m_Status));
						Results.f_AddStdOut(fg_Format("           Encryption storage: {}{\n}", Application.m_EncryptionStorage));
						Results.f_AddStdOut(fg_Format("       Encryption file system: {}{\n}", Application.m_EncryptionFileSystem));
						Results.f_AddStdOut(fg_Format("           Parent application: {}{\n}", Application.m_ParentApplication));
						Results.f_AddStdOut(fg_Format("           Self update source: {}{\n}{\n}", Application.m_bSelfUpdateSource ? "true" : "false"));
						Results.f_AddStdOut(fg_Format("                   Executable: {}{\n}", Application.m_Executable));
						Results.f_AddStdOut(fg_Format("                   Parameters: {vs,vb}{\n}", Application.m_Parameters));
						Results.f_AddStdOut(fg_Format("                  Run as user: {}{\n}", Application.m_RunAsUser));
						Results.f_AddStdOut(fg_Format("                 Run as group: {}{\n}{\n}", Application.m_RunAsGroup));

						Results.f_AddStdOut(fg_Format("                  Auto update: {}{\n}", !Application.m_AutoUpdateTags.f_IsEmpty() ? "true" : "false"));
						Results.f_AddStdOut(fg_Format("             Auto update tags: {vs}{\n}", Application.m_AutoUpdateTags));
						Results.f_AddStdOut(fg_Format("         Auto update branches: {vs}{\n}{\n}", Application.m_AutoUpdateBranches));

						Results.f_AddStdOut(fg_Format("            Pre update script: {}{\n}", Application.m_UpdateScriptPreUpdate));
						Results.f_AddStdOut(fg_Format("           Post update script: {}{\n}", Application.m_UpdateScriptPostUpdate));
						Results.f_AddStdOut(fg_Format("    Post launch update script: {}{\n}", Application.m_UpdateScriptPostLaunch));
						Results.f_AddStdOut(fg_Format("       On error update script: {}{\n}{\n}", Application.m_UpdateScriptOnError));

						Results.f_AddStdOut(fg_Format("     Version application name: {}{\n}", Application.m_VersionManagerApplication));
						Results.f_AddStdOut(fg_Format("                      Version: {}{\n}", Application.m_Version));
						Results.f_AddStdOut(fg_Format("                 Version time: {}{\n}", Application.m_VersionInfo.m_Time.f_ToLocal()));
						Results.f_AddStdOut(fg_Format("               Version config: {}{\n}", Application.m_VersionInfo.m_Configuration));
						Results.f_AddStdOut(fg_Format("                 Version size: {}{\n}", Application.m_VersionInfo.m_nBytes));
						Results.f_AddStdOut(fg_Format("                Version files: {}{\n}", Application.m_VersionInfo.m_nFiles));
						if (Application.m_VersionInfo.m_ExtraInfo.f_IsObject())
						{
							CStr InfoString = Application.m_VersionInfo.m_ExtraInfo.f_ToString("    ");
							CStr FirstLine = fg_GetStrLineSep(InfoString);
							Results.f_AddStdOut(fg_Format("                Version extra: {}{\n}", FirstLine));
							while (!InfoString.f_IsEmpty())
								Results.f_AddStdOut(fg_Format("                               {}{\n}", fg_GetStrLineSep(InfoString)));
						}
					}
				}
				Continuation.f_SetResult(fg_Move(Results));
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ListAvailableVersions(CEJSON const &_Params)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
	
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		mp_AppManagerInterface.m_pActor->f_GetAvailableVersions(_Params["Application"].f_String()) 
			> Continuation / [bVerbose, Continuation](CAppManagerInterface::CVersionsAvailableForUpdate &&_Results)
			{
				
				CDistributedAppCommandLineResults CommandLineResults;

				mint LongestApplication = fg_StrLen("Application");
				mint LongestVersion = fg_StrLen("Version");
				mint LongestPlatform = fg_StrLen("Platform");
				mint LongestConfig = fg_StrLen("Config");
				mint LongestTime = fg_StrLen("Time");
				mint LongestSize = fg_StrLen("Size");
				mint LongestFiles = fg_StrLen("Files");
				mint LongestTags = fg_StrLen("Tags");
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
					)
					{
						CommandLineResults.f_AddStdOut
							(
								fg_Format
								(
									"{sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*}   {sj*}   {sj*,a-}\n"
									, _Application
									, LongestApplication
									, _Version
									, LongestVersion
									, _Platform
									, LongestPlatform
									, _Config
									, LongestConfig
									, _Time
									, LongestTime
									, _Size
									, LongestSize
									, _Files
									, LongestFiles
									, _Tags
									, LongestTags
								)
							)
						;
					}
				;
				
				fOutputLine("Application", "Version", "Platform", "Config", "Time", "Size", "Files", "Tags");
				
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
							)
						;
						if (bVerbose && Version.m_VersionInfo.m_ExtraInfo.f_IsObject() && Version.m_VersionInfo.m_ExtraInfo.f_Object().f_OrderedIterator())
						{
							CStr JSONString = Version.m_VersionInfo.m_ExtraInfo.f_ToString("    ");
							while (!JSONString.f_IsEmpty())
							{
								CStr Line = fg_GetStrLineSep(JSONString); 
								CommandLineResults.f_AddStdOut(fg_Format("{}\n", Line));
							}
						}
					}
				}
				
				return fg_Explicit(fg_Move(CommandLineResults));
			}
		;
		
		return Continuation;
	}
}
