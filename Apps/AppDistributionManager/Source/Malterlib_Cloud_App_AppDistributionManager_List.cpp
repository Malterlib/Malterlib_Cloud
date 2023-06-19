// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/CommandLine/TableRenderer>

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

		co_return fg_Move(Versions);
	}

	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_DistributionEnum(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		CStr DistributionName = _Params["Distribution"].f_String();

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Name", "Application", "Tags", "Platforms", "Branches", "Destinations", "Rename template");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		for (auto &Distribution : mp_Distributions)
		{
			auto &Name = Distribution.f_GetName();
			if (!DistributionName.f_IsEmpty() && DistributionName != Name)
				continue;

			TCSet<CStr> DeployDestinations;

			for (auto &Destination : Distribution.m_Settings.m_DeployDestinations)
				DeployDestinations[fsp_DeployDestinationToString(Destination)];

			TableRenderer.f_AddRow
				(
					Name
					, Distribution.m_Settings.m_VersionManagerApplication
					, "{vs,vb}"_f << Distribution.m_Settings.m_Tags
					, "{vs,vb}"_f << Distribution.m_Settings.m_Platforms
					, "{vs,vb}"_f << Distribution.m_Settings.m_BranchWildcards
					, "{vs,vb}"_f << DeployDestinations
					, Distribution.m_Settings.m_RenameTemplate
				)
			;
		}

		if (!bVerbose)
		{
			TableRenderer.f_RemoveColumn(6);
			TableRenderer.f_RemoveColumn(5);
			TableRenderer.f_RemoveColumn(4);
			TableRenderer.f_RemoveColumn(3);
			TableRenderer.f_RemoveColumn(2);
			TableRenderer.f_RemoveColumn(1);
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppDistributionManagerActor::fp_CommandLine_ApplicationListAvailableVersions
		(
			CEJSONSorted const &_Params
			, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
		)
	{
		bool bVerbose = _Params["Verbose"].f_Boolean();
		auto Results = co_await fp_GetAvailableVersions(_Params["Application"].f_String());

		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Application", "Version", "Platform", "Config", "Time", "Size", "Files", "Tags", "Retry", "Extra Info");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);
		TableRenderer.f_SetAlignRight(5);
		TableRenderer.f_SetAlignRight(6);
		TableRenderer.f_SetAlignRight(8);

		for (auto &Versions : Results)
		{
			auto &ApplicationName = Results.fs_GetKey(Versions);
			for (auto &Version : Versions)
			{
				TableRenderer.f_AddRow
					(
						ApplicationName
						, Version.m_VersionID.m_VersionID
						, Version.m_VersionID.m_Platform
						, Version.m_VersionInfo.m_Configuration
						, "{tc6}"_f << Version.m_VersionInfo.m_Time.f_ToLocal()
						, "{ns }"_f << Version.m_VersionInfo.m_nBytes
						, Version.m_VersionInfo.m_nFiles
						, "{vs,vb}"_f << Version.m_VersionInfo.m_Tags
						, Version.m_VersionInfo.m_RetrySequence
						, Version.m_VersionInfo.m_ExtraInfo.f_IsValid() 
						? Version.m_VersionInfo.m_ExtraInfo.f_ToStringColored(_pCommandLine->m_AnsiFlags, "  ", EJSONDialectFlag_AllowUndefined) 
						: CStr()
					)
				;
			}
		}

		if (!bVerbose)
			TableRenderer.f_RemoveColumn(9);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}
}
