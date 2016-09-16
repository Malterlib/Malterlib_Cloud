// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_EnumApplications(CEJSON const &_Params)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CDistributedAppCommandLineResults Results;
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			Results.f_AddStdOut(fg_Format("{}{\n}", Application.m_Name));
			if (bVerbose)
			{
				Results.f_AddStdOut(fg_Format("                   Executable: {}{\n}", Application.m_Executable));
				Results.f_AddStdOut(fg_Format("                   Parameters: {vs,vb}{\n}", Application.m_ExecutableParameters));
				Results.f_AddStdOut(fg_Format("                  Run as user: {}{\n}", Application.m_RunAsUser));
				Results.f_AddStdOut(fg_Format("                 Run as group: {}{\n}", Application.m_RunAsGroup));
				Results.f_AddStdOut(fg_Format("           Encryption storage: {}{\n}", Application.m_EncryptionStorage));
				Results.f_AddStdOut(fg_Format("                       Status: {}{\n}{\n}", Application.m_LaunchState));
				
				Results.f_AddStdOut(fg_Format("                  Auto update: {}{\n}", Application.m_bAutoUpdate ? "true" : "false"));
				Results.f_AddStdOut(fg_Format("             Auto update tags: {vs}{\n}", Application.m_AutoUpdateTags));
				Results.f_AddStdOut(fg_Format("         Auto update branches: {vs}{\n}{\n}", Application.m_AutoUpdateBranches));
				
				Results.f_AddStdOut(fg_Format("            Pre update script: {}{\n}", Application.m_UpdateScripts.m_PreUpdate));
				Results.f_AddStdOut(fg_Format("           Post update script: {}{\n}", Application.m_UpdateScripts.m_PostUpdate));
				Results.f_AddStdOut(fg_Format("    Post launch update script: {}{\n}{\n}", Application.m_UpdateScripts.m_PostLaunch));
				
				Results.f_AddStdOut(fg_Format("             Application name: {}{\n}", Application.m_VersionManagerApplication));
				Results.f_AddStdOut(fg_Format("                      Version: {}{\n}", Application.m_LastInstalledVersion));
				Results.f_AddStdOut(fg_Format("                 Version time: {}{\n}", Application.m_LastInstalledVersionInfo.m_Time));
				Results.f_AddStdOut(fg_Format("               Version config: {}{\n}", Application.m_LastInstalledVersionInfo.m_Configuration));
				Results.f_AddStdOut(fg_Format("                 Version size: {}{\n}", Application.m_LastInstalledVersionInfo.m_nBytes));
				Results.f_AddStdOut(fg_Format("                Version files: {}{\n}", Application.m_LastInstalledVersionInfo.m_nFiles));
				CStr InfoString = Application.m_LastInstalledVersionInfo.m_ExtraInfo.f_ToString("    ");
				CStr FirstLine = fg_GetStrLineSep(InfoString);
				Results.f_AddStdOut(fg_Format("                Version extra: {}{\n}", FirstLine));
				while (!InfoString.f_IsEmpty())
					Results.f_AddStdOut(fg_Format("                               {}{\n}", fg_GetStrLineSep(InfoString)));
			}
		}
		return fg_Explicit(fg_Move(Results));
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ListAvailableVersions(CEJSON const &_Params)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr ApplicationName = _Params["Application"].f_String();
		CDistributedAppCommandLineResults CommandLineResults;

		mint LongestApplication = fg_StrLen("Application");
		mint LongestVersion = fg_StrLen("Version");
		mint LongestConfig = fg_StrLen("Config");
		mint LongestTime = fg_StrLen("Time");
		mint LongestSize = fg_StrLen("Size");
		mint LongestFiles = fg_StrLen("Files");
		mint LongestTags = fg_StrLen("Tags");
		for (auto &Application : mp_VersionManagerApplications)
		{
			LongestApplication = fg_Max(LongestApplication, Application.f_GetApplicationName().f_GetLen());
			for (auto &Version : Application.m_VersionsByTime)
			{
				LongestVersion = fg_Max(LongestVersion, fg_Format("{}", Version.f_GetVersionID()).f_GetLen());
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
							"{sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*}   {sj*}   {sj*,a-}\n"
							, _Application
							, LongestApplication
							, _Version
							, LongestVersion
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
		
		fOutputLine("Application", "Version", "Config", "Time", "Size", "Files", "Tags");
		
		for (auto &Application : mp_VersionManagerApplications)
		{
			if (!ApplicationName.f_IsEmpty() && Application.f_GetApplicationName() != ApplicationName)
				continue;
			for (auto &Version : Application.m_VersionsByTime)
			{
				fOutputLine
					(
						Application.f_GetApplicationName()
						, Version.f_GetVersionID()
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
}
