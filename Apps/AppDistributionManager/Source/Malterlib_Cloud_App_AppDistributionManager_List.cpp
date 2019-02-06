// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	auto CAppDistributionManagerActor::fp_GetAvailableVersions(CStr const &_Application) -> TCFuture<CVersionsAvailableForUpdate>
	{
		auto Auditor = f_Auditor();
		
		TCMap<CStr, TCVector<CApplicationVersion>> Versions;

		for (auto &Application : mp_VersionManagerApplications)
		{
			auto const &Name = Application.f_GetApplicationName();
			if (!_Application.f_IsEmpty() && Name != _Application)
				continue;

			auto &OutVersions = Versions[Name];
			for (auto &Version : Application.m_VersionsByTime)
			{
				auto &OutVersion = OutVersions.f_Insert();
				OutVersion.m_VersionID = Version.f_GetVersionID();
				OutVersion.m_VersionInfo = Version.m_VersionInfo;
			}
		}

		Auditor.f_Info("Enum distribution versions");

		return fg_Explicit(fg_Move(Versions));
	}

	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionEnum(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr DistributionName = _Params["Distribution"].f_String();

		TCPromise<uint32> Promise;
		for (auto &Distribution : mp_Distributions)
		{
			auto &Name = Distribution.f_GetName();
			if (!DistributionName.f_IsEmpty() && DistributionName != Name)
				continue;

			CStr DistributionInfo;
			DistributionInfo += "{}{\n}"_f << Name;
			if (bVerbose)
			{
				DistributionInfo += "      Version application name: {}{\n}{\n}"_f << Distribution.m_Settings.m_VersionManagerApplication;
				DistributionInfo += "                 Required tags: {vs}{\n}"_f << Distribution.m_Settings.m_Tags;
				DistributionInfo += "                     Platforms: {vs}{\n}"_f << Distribution.m_Settings.m_Platforms;
				DistributionInfo += "    Allowed branches wildcards: {vs}{\n}"_f << Distribution.m_Settings.m_BranchWildcards;
				DistributionInfo += "           Deploy destinations: {vs}{\n}"_f << Distribution.m_Settings.m_DeployDestinations;
				DistributionInfo += "               Rename template: {}{\n}"_f << Distribution.m_Settings.m_RenameTemplate;
			}

			*_pCommandLine += DistributionInfo;
		}

		return fg_Explicit(0);
	}
	
	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_ApplicationListAvailableVersions
		(
		 	CEJSON const &_Params
		 	, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
	
		TCPromise<uint32> Promise;
		fp_GetAvailableVersions(_Params["Application"].f_String())
			> Promise / [=](CVersionsAvailableForUpdate &&_Results)
			{
				smint LongestDistribution = fg_StrLen("Distribution");
				smint LongestVersion = fg_StrLen("Version");
				smint LongestPlatform = fg_StrLen("Platform");
				smint LongestConfig = fg_StrLen("Config");
				smint LongestTime = fg_StrLen("Time");
				smint LongestSize = fg_StrLen("Size");
				smint LongestFiles = fg_StrLen("Files");
				smint LongestTags = fg_StrLen("Tags");
				for (auto &Versions : _Results)
				{
					LongestDistribution = fg_Max(LongestDistribution, _Results.fs_GetKey(Versions).f_GetLen());
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
						auto const &_Distribution
						, auto const &_Version
						, auto const &_Platform
						, auto const &_Config
						, auto const &_Time
						, auto const &_Size
						, auto const &_Files
						, auto const &_Tags
					)
					{
						*_pCommandLine += "{sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*,a-}   {sj*}   {sj*}   {sj*,a-}\n"_f
							<< _Distribution
							<< LongestDistribution
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
						;
					}
				;
				
				fOutputLine("Distribution", "Version", "Platform", "Config", "Time", "Size", "Files", "Tags");
				
				for (auto &Versions : _Results)
				{
					auto &DistributionName = _Results.fs_GetKey(Versions);
					for (auto &Version : Versions)
					{
						fOutputLine
							(
								DistributionName
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
								*_pCommandLine += "{}\n"_f << Line;
							}
						}
					}
				}
				
				Promise.f_SetResult(0);
			}
		;
		
		return Promise.f_MoveFuture();
	}
}
