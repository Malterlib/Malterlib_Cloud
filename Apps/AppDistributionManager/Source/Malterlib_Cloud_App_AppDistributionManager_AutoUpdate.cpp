// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/RandomID>

#include "Malterlib_Cloud_App_AppDistributionManager.h"

namespace NMib::NCloud::NAppDistributionManager
{
	bool CAppDistributionManagerActor::fsp_VersionMatchesSettings(CVersionManagerVersion const &_Version, CDistributionSettings const &_Settings)
	{
		if (_Settings.m_Tags.f_IsEmpty())
			return false;

		for (auto &Tag : _Settings.m_Tags)
		{
			if (!_Version.m_VersionInfo.m_Tags.f_FindEqual(Tag))
				return false;
		}

		if (!_Settings.m_BranchWildcards.f_IsEmpty())
		{
			bool bFoundMatch = false;
			for (auto &Wildcard : _Settings.m_BranchWildcards)
			{
				if (fg_StrMatchWildcard(_Version.f_GetVersionID().m_VersionID.m_Branch.f_GetStr(), Wildcard.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
				{
					bFoundMatch = true;
					break;
				}
			}
			if (!bFoundMatch)
				return false;
		}

		if (!_Settings.m_Platforms.f_IsEmpty())
		{
			if (!_Settings.m_Platforms.f_FindEqual(_Version.f_GetVersionID().m_Platform))
				return false;
		}

		return true;
	}

	TCFuture<TCSet<CStr>> CAppDistributionManagerActor::fp_AutoUpdate_DeployDistribution
		(
			CStr _DistributionName
			, CVersionManager::CVersionIDAndPlatform _VersionID
			, CVersionManager::CVersionInformation _VersionInfo
			, TCSet<EDeployDestination> _DeployDestinations
		)
	{
		auto SequenceSubscription = co_await mp_DistributeSequencer.f_Sequence();

		CDistribution *pDistribution;
		CDistribution *pInitialDistribution = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pDistribution = mp_Distributions.f_FindEqual(_DistributionName);
					if (!pDistribution || (pInitialDistribution && pDistribution != pInitialDistribution))
						return DMibErrorInstance("Distribution no longer exists");

					pInitialDistribution = pDistribution;

					if (mp_State.m_bStoppingApp)
						return DMibErrorInstance("Stopping app");

					return nullptr;
				}
			)
		;

		CStr DownloadDirectory = CFile::fs_GetProgramDirectory() / "Temp" / fg_RandomID();
		CApplicationVersion ApplicationVersion = {_VersionID, _VersionInfo};

		auto &Distribution = *pDistribution;
		auto const &DistributionSettings = Distribution.m_Settings;
		auto const &VersionManagerApplicationName = DistributionSettings.m_VersionManagerApplication;

		auto CleanupDownload = g_BlockingActorSubscription / [=]()
			{
				try
				{
					if (CFile::fs_FileExists(DownloadDirectory))
						CFile::fs_DeleteDirectoryRecursive(DownloadDirectory);
				}
				catch (CException const &_Exception)
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppDistributionManager
							, Error
							, "Failed to clean up downloaded application '{}' ({}) version '{}': {}"
							, _DistributionName
							, VersionManagerApplicationName
							, _VersionID
							, _Exception
						)
					;
				}
			}
		;

		auto AsyncDestroyDownload = co_await fg_AsyncDestroy(fg_Move(CleanupDownload));

		auto VersionInformation = co_await
			(
				fp_DownloadApplication(VersionManagerApplicationName, _VersionID, DownloadDirectory)
				% ("Failed to download '{}' ({}) version '{}'"_f << _DistributionName << VersionManagerApplicationName << _VersionID)
			)
		;

		if (VersionInformation.m_Files.f_GetLen() != 1)
			co_return DMibErrorInstance("Version must contain exactly 1 file. {} files found"_f << VersionInformation.m_Files.f_GetLen());

		CStr SourceFile = VersionInformation.m_Files[0];
		CStr SourcePath = DownloadDirectory / SourceFile;

		auto &RunningDeploys = Distribution.m_RunningDeploys[_VersionID];

		auto CleanupDeploy = g_ActorSubscription / [_DistributionName, _VersionID, this]() -> TCFuture<void>
			{
				CDistribution *pDistribution;
				auto OnResume = co_await fg_OnResume
					(
						[&]() -> CExceptionPointer
						{
							pDistribution = mp_Distributions.f_FindEqual(_DistributionName);
							if (!pDistribution)
								return DMibErrorInstance("Distribution no longer exists");

							return nullptr;
						}
					)
				;

				auto &Distribution = *pDistribution;

				TCFutureVector<void> Destroys;
				for (auto &RunningDeploy : Distribution.m_RunningDeploys[_VersionID])
					fg_Move(RunningDeploy).f_Destroy() > Destroys;

				co_await fg_AllDone(Destroys).f_Wrap() > fg_LogError("DeployDistribution", "Failed to destroy running deploy");

				pDistribution->m_RunningDeploys.f_Remove(_VersionID);

				fp_AutoUpdate_Update();

				co_return {};
			}
		;

		auto AsyncDestroyDeploy = co_await fg_AsyncDestroy(fg_Move(CleanupDeploy));

		TCFutureMap<CStr, void> DeploysResults;

		for (auto &DeployDestination : _DeployDestinations)
		{
			CStr DeployDestinationName = fsp_DeployDestinationToString(DeployDestination);
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppDistributionManager
					, Info
					, "Deploying '{}' ({}) version '{}' to '{}'"
					, _DistributionName
					, VersionManagerApplicationName
					, _VersionID
					, DeployDestinationName
				)
			;

			auto DeployDestinationActor = fp_CreateDeploy(DeployDestination);
			RunningDeploys.f_Insert(DeployDestinationActor);

			CDeployInfo DeployInfo;
			DeployInfo.m_SourceFile = SourcePath;
			DeployInfo.m_Version = ApplicationVersion;
			DeployInfo.m_Settings = DistributionSettings;

			{
				CStr RenamedTemplate = DistributionSettings.m_RenameTemplate;

				RenamedTemplate = RenamedTemplate.f_Replace("{Name", "{0");
				RenamedTemplate = RenamedTemplate.f_Replace("{VersionAppName", "{1");
				RenamedTemplate = RenamedTemplate.f_Replace("{PlatformFull", "{2");
				RenamedTemplate = RenamedTemplate.f_Replace("{PlatformFamily", "{4");
				RenamedTemplate = RenamedTemplate.f_Replace("{PlatformArchitecture", "{5");
				RenamedTemplate = RenamedTemplate.f_Replace("{Platform", "{3");
				RenamedTemplate = RenamedTemplate.f_Replace("{VersionBranch", "{7");
				RenamedTemplate = RenamedTemplate.f_Replace("{VersionMajor", "{8");
				RenamedTemplate = RenamedTemplate.f_Replace("{VersionMinor", "{9");
				RenamedTemplate = RenamedTemplate.f_Replace("{VersionRevision", "{10");
				RenamedTemplate = RenamedTemplate.f_Replace("{Version", "{6");
				RenamedTemplate = RenamedTemplate.f_Replace("{FileExtension", "{13");
				RenamedTemplate = RenamedTemplate.f_Replace("{FileName", "{12");
				RenamedTemplate = RenamedTemplate.f_Replace("{File", "{11");

				CStr ParsePlatform = ApplicationVersion.m_VersionID.m_Platform;
				CStr Platform = fg_GetStrSep(ParsePlatform, "-");
				if (Platform == "electron")
				{
					DeployInfo.m_bElectron = true;
					Platform = fg_GetStrSep(ParsePlatform, "-");
					DeployInfo.m_ElectronPlatform = Platform;
				}
				CStr PlatformArchitecture = fg_GetStrSep(ParsePlatform, "-");
				ch8 const *pPlatformParse = Platform;
				fg_ParseAlpha(pPlatformParse);
				CStr PlatformFamily(Platform.f_GetStr(), pPlatformParse - Platform.f_GetStr());
				if (PlatformFamily == "OSX")
					PlatformFamily = "macOS";

				CStr ParseSourceFile = SourceFile;
				CStr SourceFileName = fg_GetStrSep(ParseSourceFile, ".");
				CStr SourceFileExtension = ParseSourceFile;

				DeployInfo.m_Renamed = CStr::CFormat(RenamedTemplate)
					<< _DistributionName
					<< VersionManagerApplicationName
					<< ApplicationVersion.m_VersionID.m_Platform
					<< Platform
					<< PlatformFamily
					<< PlatformArchitecture
					<< CStr("{}.{}.{}"_f << _VersionID.m_VersionID.m_Major << _VersionID.m_VersionID.m_Minor << _VersionID.m_VersionID.m_Revision)
					<< _VersionID.m_VersionID.m_Branch
					<< _VersionID.m_VersionID.m_Major
					<< _VersionID.m_VersionID.m_Minor
					<< _VersionID.m_VersionID.m_Revision
					<< SourceFile
					<< SourceFileName
					<< SourceFileExtension
				;
			}

			DeployDestinationActor(&CDeployDestination::f_Deploy, DeployInfo)
				> DeploysResults[DeployDestinationName]
			;
		}

		auto Results = co_await fg_AllDoneWrapped(DeploysResults);

		CStr DeployFailures;
		TCSet<CStr> SuccessFullDeploys;
		for (auto &Result : Results.f_Entries())
		{
			CStr const &DeployDestinationName = Result.f_Key();
			if (!Result.f_Value())
				fg_AddStrSep(DeployFailures, "\tFailed to deploy to '{}': {}"_f << DeployDestinationName << Result.f_Value().f_GetExceptionStr(), "\n");
			else
				SuccessFullDeploys[DeployDestinationName];
		}

		if (DeployFailures.f_IsEmpty())
		{
			Distribution.m_DeployedVersions[_VersionID] = {ApplicationVersion.m_VersionInfo.m_Time, ApplicationVersion.m_VersionInfo.m_RetrySequence};

			fp_SaveState(Distribution);

			co_await (mp_State.m_StateDatabase.f_Save() % "Failed to save state");

			co_return fg_Move(SuccessFullDeploys);
		}
		else
		{
			if (!SuccessFullDeploys.f_IsEmpty())
			{
				co_return DMibErrorInstance
					(
						"Successfully deployed to {vs}, but failed to deploy to some deploy destinations:\n{}"_f
						<< SuccessFullDeploys
						<< DeployFailures
					)
				;
			}
			else
				co_return DMibErrorInstance("Failed to deploy to all deploy destinations:\n{}"_f << DeployFailures);
		}
	}

	void CAppDistributionManagerActor::fp_AutoUpdate_Update()
	{
		if (mp_State.m_bStoppingApp)
			return;
			
		for (auto &Distribution : mp_Distributions)
		{
			CStr const &DistributionName = Distribution.f_GetName();
			auto const &DistributionSettings = Distribution.m_Settings;
			auto const &VersionManagerApplicationName = DistributionSettings.m_VersionManagerApplication;

			auto pVersionManagerApplication = mp_VersionManagerApplications.f_FindEqual(VersionManagerApplicationName);
			if (!pVersionManagerApplication)
				continue;

			auto &VersionManagerApplication = *pVersionManagerApplication;

			for (auto &Version : VersionManagerApplication.m_VersionsByTime)
			{
				if (!fsp_VersionMatchesSettings(Version, DistributionSettings))
					continue;

				auto &VersionID = Version.f_GetVersionID();
				auto &VersionInfo = Version.m_VersionInfo;

				if
					(
						auto *pPreviouslyDistributed = Distribution.m_DeployedVersions.f_FindEqual(VersionID)
						; pPreviouslyDistributed
						&& pPreviouslyDistributed->m_RetrySequence == VersionInfo.m_RetrySequence
						&& pPreviouslyDistributed->m_Time == VersionInfo.m_Time
					)
				{
					continue;
				}

				if
					(
						auto *pPreviouslyDistributed = Distribution.m_TriedDeployVersions.f_FindEqual(VersionID)
						; pPreviouslyDistributed && *pPreviouslyDistributed == VersionInfo
					)
				{
					continue;
				}
				Distribution.m_TriedDeployVersions[VersionID] = VersionInfo;

				TCSet<EDeployDestination> DeployDestinations = DistributionSettings.m_DeployDestinations;

				if (DeployDestinations.f_IsEmpty())
				{
					if (auto pAppDistribution = VersionInfo.m_ExtraInfo.f_GetMember("AppDistribution", EJsonType_Object))
					{
						if (auto pValue = pAppDistribution->f_GetMember("DeployDestinations", EJsonType_Array))
						{
							for (auto &TypeJson : pValue->f_Array())
							{
								try
								{
									DeployDestinations[fsp_DeployDestinationFromString(TypeJson.f_String())];
								}
								catch (CException const &_Exception)
								{
									DMibLogWithCategory(Malterlib/Cloud/AppDistributionManager, Error, "Invalid 'DeployDestinations' in version extra info: {}", _Exception);
								}
							}
						}
					}
				}

				if (DeployDestinations.f_IsEmpty())
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppDistributionManager
							, Info
							, "Skipped deploy of '{}' ({}) version '{}' due to no deploy types specified"
							, DistributionName
							, VersionManagerApplicationName
							, VersionID
						)
					;
					continue;
				}

				if (!Distribution.m_RunningDeploys(VersionID).f_WasCreated())
					continue;

				DMibLogWithCategory
					(
						Malterlib/Cloud/AppDistributionManager
						, Info
						, "Downloading '{}' ({}) version '{}'"
						, DistributionName
						, VersionManagerApplicationName
						, VersionID
					)
				;

				fp_AutoUpdate_DeployDistribution(DistributionName, VersionID, VersionInfo, DeployDestinations) > [=](TCAsyncResult<TCSet<CStr>> &&_SuccessFullDeploys)
					{
						if (!_SuccessFullDeploys)
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppDistributionManager
									, Error
									, "Failed to deploy '{}' ({}) version '{}': {}"
									, DistributionName
									, VersionManagerApplicationName
									, VersionID
									, _SuccessFullDeploys.f_GetExceptionStr()
								)
							;
						}
						else
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppDistributionManager
									, Info
									, "Finished deploying '{}' ({}) version '{}' to {vs}"
									, DistributionName
									, VersionManagerApplicationName
									, VersionID
									, *_SuccessFullDeploys
								)
							;
						}
					}
				;
			}
		}
	}
}
