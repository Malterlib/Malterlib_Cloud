// Copyright © 2018 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
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
					if (auto pAppDistribution = VersionInfo.m_ExtraInfo.f_GetMember("AppDistribution", EJSONType_Object))
					{
						if (auto pValue = pAppDistribution->f_GetMember("DeployDestinations", EJSONType_Array))
						{
							for (auto &TypeJSON : pValue->f_Array())
							{
								try
								{
									DeployDestinations[fsp_DeployDestinationFromString(TypeJSON.f_String())];
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

				auto CleanupDeploy = g_OnScopeExitActor > [=]
					{
						TCActorResultVector<void> Destroys;
						for (auto &RunningDeploy : Distribution.m_RunningDeploys[VersionID])
							RunningDeploy->f_Destroy() > Destroys.f_AddResult();
						Destroys.f_GetResults() > [=](TCAsyncResult<TCVector<TCAsyncResult<void>>> &&)
							{
								auto pDistribution = mp_Distributions.f_FindEqual(DistributionName);
								if (!pDistribution)
									return;

								pDistribution->m_RunningDeploys.f_Clear();
								fp_AutoUpdate_Update();
							}
						;
					}
				;

				CApplicationVersion ApplicationVersion = {Version.f_GetVersionID(), VersionInfo};

				CStr DownloadDirectory = CFile::fs_GetProgramDirectory() / "Temp" / fg_RandomID();

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

				mp_DistributeSequencer / [=]() -> TCFuture<TCSet<CStr>>
					{
						if (!mp_Distributions.f_FindEqual(DistributionName))
							return DMibErrorInstance("Distribution no longer exists");
						if (mp_State.m_bStoppingApp)
							return DMibErrorInstance("Stopping app");

						auto CleanupDownload = g_OnScopeExitActor > [=]
							{
								if (!mp_FileActor)
									return;

								g_Dispatch(mp_FileActor) / [=]
									{
										if (CFile::fs_FileExists(DownloadDirectory))
											CFile::fs_DeleteDirectoryRecursive(DownloadDirectory);
									}
									> [=](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
										{
											DMibLogWithCategory
												(
													Malterlib/Cloud/AppDistributionManager
													, Error
												 	, "Failed to clean up downloaded application '{}' ({}) version '{}': {}"
													, DistributionName
												  	, VersionManagerApplicationName
													, VersionID
												 	, _Result.f_GetExceptionStr()
												)
											;
										}
									}
								;
							}
						;

						TCPromise<TCSet<CStr>> Promise;
						fp_DownloadApplication(VersionManagerApplicationName, VersionID, DownloadDirectory)
							> Promise
							% ("Failed to download '{}' ({}) version '{}'"_f << DistributionName << VersionManagerApplicationName << VersionID)
							/ [=](CVersionInformation &&_VersionInformation)
							{
								auto pDistribution = mp_Distributions.f_FindEqual(DistributionName);
								if (!pDistribution)
									return Promise.f_SetException(DMibErrorInstance("Distribution no longer exists"));
								if (mp_State.m_bStoppingApp)
									return Promise.f_SetException(DMibErrorInstance("Stopping app"));
								if (_VersionInformation.m_Files.f_GetLen() != 1)
									return Promise.f_SetException(DMibErrorInstance("Version must contain exactly 1 file. {} files found"_f << _VersionInformation.m_Files.f_GetLen()));

								CStr SourceFile = _VersionInformation.m_Files[0];
								CStr SourcePath = DownloadDirectory / SourceFile;

								auto &Distribution = *pDistribution;
								auto &RunningDeploys = Distribution.m_RunningDeploys[VersionID];

								TCActorResultMap<CStr, void> DeploysResults;

								for (auto &DeployDestination : DeployDestinations)
								{
									CStr DeployDestinationName = fsp_DeployDestinationToString(DeployDestination);
									DMibLogWithCategory
										(
											Malterlib/Cloud/AppDistributionManager
											, Info
										 	, "Deploying '{}' ({}) version '{}' to '{}'"
											, DistributionName
										 	, VersionManagerApplicationName
											, VersionID
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
											<< DistributionName
											<< VersionManagerApplicationName
											<< ApplicationVersion.m_VersionID.m_Platform
											<< Platform
											<< PlatformFamily
											<< PlatformArchitecture
											<< CStr("{}.{}.{}"_f << VersionID.m_VersionID.m_Major << VersionID.m_VersionID.m_Minor << VersionID.m_VersionID.m_Revision)
											<< VersionID.m_VersionID.m_Branch
											<< VersionID.m_VersionID.m_Major
											<< VersionID.m_VersionID.m_Minor
											<< VersionID.m_VersionID.m_Revision
											<< SourceFile
											<< SourceFileName
											<< SourceFileExtension
										;
									}

									DeployDestinationActor(&CDeployDestination::f_Deploy, DeployInfo)
										> DeploysResults.f_AddResult(DeployDestinationName)
									;
								}

								DeploysResults.f_GetResults() > [=](TCAsyncResult<TCMap<CStr, TCAsyncResult<void>>> &&_Results)
									{
										auto pDistribution = mp_Distributions.f_FindEqual(DistributionName);
										if (!pDistribution)
											return Promise.f_SetException(DMibErrorInstance("Distribution no longer exists"));
										if (mp_State.m_bStoppingApp)
											return Promise.f_SetException(DMibErrorInstance("Stopping app"));

										auto &Distribution = *pDistribution;

										// Reference cleanups at innermost
										(void)CleanupDownload;
										(void)CleanupDeploy;

										if (!_Results)
											return Promise.f_SetException(_Results);

										CStr DeployFailures;
										TCSet<CStr> SuccessFullDeploys;
										for (auto &Result : *_Results)
										{
											CStr const &DeployDestinationName = _Results->fs_GetKey(Result);
											if (!Result)
												fg_AddStrSep(DeployFailures, "\tFailed to deploy to '{}': {}"_f << DeployDestinationName << Result.f_GetExceptionStr(), "\n");
											else
												SuccessFullDeploys[DeployDestinationName];
										}

										if (DeployFailures.f_IsEmpty())
										{
											Distribution.m_DeployedVersions[VersionID] = {ApplicationVersion.m_VersionInfo.m_Time, ApplicationVersion.m_VersionInfo.m_RetrySequence};

											fp_SaveState(Distribution);

											mp_State.m_StateDatabase.f_Save() > Promise % "Failed to save state" / [=]
												{
													Promise.f_SetResult(SuccessFullDeploys);
												}
											;
										}
										else
										{
											if (!SuccessFullDeploys.f_IsEmpty())
											{
												Promise.f_SetException
													(
													 	DMibErrorInstance
													 	(
															"Successfully deployed to {vs}, but failed to deploy to some deploy destinations:\n{}"_f
														 	<< SuccessFullDeploys
														 	<< DeployFailures
														)
													)
												;
											}
											else
												Promise.f_SetException(DMibErrorInstance("Failed to deploy to all deploy destinations:\n{}"_f << DeployFailures));
										}
									}
								;
							}
						;
						return Promise.f_MoveFuture();
					}
					> [=](TCAsyncResult<TCSet<CStr>> &&_SuccessFullDeploys)
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
