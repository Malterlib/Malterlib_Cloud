// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Cloud/FileTransfer>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_StartPendingSelfUpdateReporting(CPendingSelfupdate const &_Pending)
	{
		auto *pApplication = mp_Applications.f_FindEqual(_Pending.m_Name);
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Could not find pending self update application: {}", _Pending.m_Name);
			return;
		}

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Starting pending self update reporting");

		mp_bPendingSelfUpdateInProgress = true;
		auto CleanupAutoUpdate = g_OnScopeExitActor / [this]
			{
				mp_bPendingSelfUpdateInProgress = false;
				fp_AutoUpdate_Update();
			}
		;

		TCSharedPointer<NTime::CStopwatch> pStopwatch = fg_Construct(true);

		pStopwatch->f_AddOffset((_Pending.m_StartUpdateTime - CTime::fs_NowUTC()).f_GetSecondsFraction());

		TCSharedPointerSupportWeak<CUpdateApplicationState> pUpdateState = fg_Construct();
		pUpdateState->m_pApplication = *pApplication;
		pUpdateState->m_UniqueUpdateID = _Pending.m_UniqueUpdateID;
		pUpdateState->m_pStopwatch = pStopwatch;
		pUpdateState->m_StartUpdateTime = _Pending.m_StartUpdateTime;
		pUpdateState->m_VersionID = _Pending.m_VersionID;
		pUpdateState->m_VersionTime = _Pending.m_VersionTime;
		pUpdateState->m_VersionRetrySequence = _Pending.m_VersionRetrySequence;
		pUpdateState->m_bSetLastTried = true;
		pUpdateState->m_fOnInfo = [Name = (*pApplication)->m_Name](CStr const &_Info)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pending self update '{}': {}", Name, _Info);
			}
		;

		// The update events hold the state until they have recorded the result
		mp_pPendingSelfUpdate = pUpdateState.f_Weak();

		// Automatic updates stay blocked until the event has recorded the result, since an installed version counts as finished only then
		auto fOnRecorded = [CleanupAutoUpdate](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to record the result of the pending self update: {}", _Result.f_GetExceptionStr());
			}
		;

		fg_Move(mp_InitialStartupResultFuture) > [this, pUpdateState, fOnRecorded](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					CStr Error = fg_Format("Pending self update failed: {}", fsp_LimitErrorLogSize(_Result.f_GetExceptionStr(), 128));
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "{}", Error);
					fp_OnUpdateEvent(pUpdateState, EUpdateStage::EUpdateStage_Failed, Error) > fg_TempCopy(fOnRecorded);
					return;
				}

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pending self update finished");
				fp_OnUpdateEvent(pUpdateState, EUpdateStage::EUpdateStage_Finished, {}) > fg_TempCopy(fOnRecorded);
			}
		;
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_DeferToNextRestart(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState, TCSharedPointer<CCanDestroyTracker> _pCanDestroy)
	{
		auto &Application = *_pState->m_pApplication;

		CEJsonSorted PendingSelfUpdate =
			{
				"Name"_= Application.m_Name
				, "VersionID"_= _pState->m_VersionID.f_ToJson()
				, "VersionTime"_= _pState->m_VersionTime
				, "VersionRetrySequence"_= _pState->m_VersionRetrySequence
				, "StartUpdateTime"_= _pState->m_StartUpdateTime
				, "UniqueUpdateID"_= _pState->m_UniqueUpdateID
			}
		;

		mp_State.m_StateDatabase.m_Data["PendingSelfUpdateProcess"] = CEJsonSortedYaml::fs_FromCompatible(fg_Move(PendingSelfUpdate));

		co_await (fp_SaveStateDatabase() % "Failed save state database");
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplicationRunProcess(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_None, {});
		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_SyncStart, {});

		_pState->m_fOnInfo("Change encryption");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_ChangeEncryption, {});

		if (auto pException = _pState->f_CheckAbort())
			co_return pException;

		co_await (fp_ChangeEncryption(_pState->m_pApplication, EEncryptOperation_Open, false) % "Failed to open encryption");

		_pState->m_bUnencrypted = true;

		co_await (fp_UpdateApplication_DownloadVersion(_pState) % "Failed to download version");
		co_await (fp_UpdateApplication_Unpack(_pState) % "Failed to unpack application");

		if (_pState->m_bDryRun)
		{
			_pState->m_fOnInfo("Skipping stop, update and restart because of dry run");
			co_return {};
		}

		co_await (fp_UpdateApplication_StopOldApp(_pState) % "Failed to stop old app");
		co_await (fp_UpdateApplication_PreUpdate(_pState) % "Failed pre update app");
		co_await (fp_UpdateApplication_UpdateApplicationFiles(_pState) % "Failed to update application files");
		co_await (fp_UpdateApplication_SaveApplicationState(_pState) % "Failed to save application state");
		co_await (fp_UpdateApplication_PostUpdate(_pState) % "Failed post update");

		{
			auto &Application = *_pState->m_pApplication;
			Application.m_LastMigratedVersion = Application.m_LastInstalledVersion;
			Application.m_LastMigratedVersionInfo = Application.m_LastInstalledVersionInfo;
			co_await (fp_UpdateApplicationJson(_pState->m_pApplication) % "Failed to save application state");
		}

		auto pCanDestroy = mp_pCanDestroy;
		if (!pCanDestroy)
			co_return DErrorInstance("Application stopped");

		bool bQuitManager = co_await (fp_UpdateApplication_StartNewApp(_pState) % "Failed to start new app. Will retry periodically");

		if (!_pState->m_pApplication->m_bDeleted && bQuitManager)
			co_await fp_UpdateApplication_DeferToNextRestart(_pState, pCanDestroy);
		else
			co_await (fp_UpdateApplication_PostLaunch(_pState) % "Failed post launch");

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_DownloadVersion(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		CVersionManager::CVersionInformation VersionInfo;

		if (!State.m_SourcePath.f_IsEmpty())
		{
			// Only the version info tells the platform of the package, which an agent distribution must match
			CStr AgentPlatform = State.m_pApplication->f_AgentDistributionPlatform();

			// The package that the operator names stays unchanged while the update reads it, here for its version info and in
			// fp_UpdateApplication_Unpack for its files
			auto PackageInfo = co_await fp_ReadPackageVersionInfo(State.m_SourcePath, State.m_fOnInfo);
			if (!PackageInfo)
			{
				if (!AgentPlatform.f_IsEmpty())
					co_return DMibErrorInstance("The package has no readable version info, which application '{}' needs for its agent platform"_f << State.m_pApplication->m_Name);

				co_return {};
			}

			// The time of a package depends on how its files were written, and an upload can record another. A package of the installed version
			// keeps the installed time, so that the version manager doesn't offer the same version as newer
			auto &Installed = *State.m_pApplication;
			if (PackageInfo->m_VersionID == Installed.m_LastInstalledVersion && Installed.m_LastInstalledVersionInfo.m_Time.f_IsValid())
				PackageInfo->m_VersionInfo.m_Time = Installed.m_LastInstalledVersionInfo.m_Time;

			// The versions of another application don't compare with those the update source offers, so such a package installs its files without
			// its version info and settings, and an agent distribution refuses it
			auto &VersionManagerApplication = State.m_pApplication->m_Settings.m_VersionManagerApplication;
			if (!VersionManagerApplication.f_IsEmpty() && PackageInfo->m_Application != VersionManagerApplication)
			{
				CStr Mismatch
					= "The package is for application '{}', but application '{}' updates from version manager application '{}'"_f
					<< PackageInfo->m_Application
					<< State.m_pApplication->m_Name
					<< VersionManagerApplication
				;

				if (!AgentPlatform.f_IsEmpty())
					co_return DMibErrorInstance(Mismatch);

				State.m_fOnInfo("{}, so its version info isn't applied"_f << Mismatch);
				co_return {};
			}

			if (!AgentPlatform.f_IsEmpty() && PackageInfo->m_VersionID.m_Platform != AgentPlatform)
			{
				co_return DMibErrorInstance
					(
						"The package is for platform '{}', but application '{}' provides the agent for platform '{}'"_f
						<< PackageInfo->m_VersionID.m_Platform
						<< State.m_pApplication->m_Name
						<< AgentPlatform
					)
				;
			}

			// Against the installed version, the time only tells packages of the same version ID apart; fp_FindVersion compares the IDs
			State.m_VersionID = PackageInfo->m_VersionID;
			State.m_VersionTime = PackageInfo->m_VersionInfo.m_Time;
			VersionInfo = fg_Move(PackageInfo->m_VersionInfo);
		}
		else
		{
			State.m_fOnInfo(fg_Format("Downloading version '{}' from version managers", State.m_VersionID));

			co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_DownloadVersion, {});

			if (auto pException = State.f_CheckAbort())
				co_return pException;

			// An application whose storage the environment manages has no host application directory, so the download stages in the host-side environment storage
			if (State.m_pRemoteStorageEnvironment)
			{
				if (CStr Error = fp_CheckEnvironmentStagingStorage(*State.m_pRemoteStorageEnvironment); !Error.f_IsEmpty())
					co_return DMibErrorInstance(Error);
			}

			CStr DownloadDirectoryRoot;
			if (State.m_pRemoteStorageEnvironment)
				DownloadDirectoryRoot = fp_GetEnvironmentStorageDirectory(*State.m_pRemoteStorageEnvironment) / ".AppStage" / State.m_pApplication->m_Name / "Download";
			else
				DownloadDirectoryRoot = State.m_pApplication->f_GetDirectory() / "TempVersionDownload";
			CStr DownloadDirectory = DownloadDirectoryRoot / fg_FastRandomID();
			State.m_SourcePath = DownloadDirectory;
			State.m_AllowSourceExist[DownloadDirectoryRoot];

			State.m_DownloadDirectoryCleanup = g_BlockingActorSubscription / [DownloadDirectoryRoot]
				{
					try
					{
						if (CFile::fs_FileExists(DownloadDirectoryRoot))
							CFile::fs_DeleteDirectoryRecursive(DownloadDirectoryRoot);
					}
					catch (CExceptionFile const &_Exception)
					{
						[[maybe_unused]] auto &Exception = _Exception;
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up version download: {}", Exception);
					}
				}
			;

			VersionInfo = co_await fp_DownloadApplication(State.m_pApplication->m_Settings.m_VersionManagerApplication, State.m_VersionID, DownloadDirectory);

			State.m_fOnInfo(fg_Format("Application downloaded, unpacking"));
		}

		auto &Application = *State.m_pApplication;

		if (Application.m_bDeleted)
			co_return DMibErrorInstance("Application has been deleted, aborting");

		State.m_pVersionInfo = fg_Construct(VersionInfo);

		for (auto &Tag : State.m_RequiredTags)
		{
			if (!VersionInfo.m_Tags.f_FindEqual(Tag))
				co_return DMibErrorInstance(fg_Format("Missing required tag: '{}'", Tag));
		}

		if (State.m_bDryRun)
			co_return {};

		// The settings in the version info of an agent distribution describe the guest platform and must not be applied on the host
		bool bAgentDistribution = !Application.f_AgentDistributionPlatform().f_IsEmpty();

		if (State.m_bUpdateSettings && !bAgentDistribution)
		{
			CApplicationSettings NewSettings = Application.m_Settings;
			EApplicationSetting NewChangedSettings = EApplicationSetting_None;

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to get settings from version info");
				NewSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
			}

#ifdef DPlatformFamily_Windows
			// A new run-as account gets a password of its own. The update can't change the password of an existing account, so it takes the password
			// of the version info or of an application that runs as it, and is refused without one
			if (!NewSettings.m_RunAsUser.f_IsEmpty() && NewSettings.m_RunAsUser != Application.m_Settings.m_RunAsUser)
			{
				bool bAccountExists = false;
				{
					auto CaptureScope = co_await (g_CaptureExceptions % "Failed to look up the account of the run-as user");

					CStr UserID;
					bAccountExists = NSys::fg_UserManagement_UserExists(mp_pUniqueUserGroup->f_GetUser(NewSettings.m_RunAsUser), UserID);
				}

				if (!bAccountExists)
					NewSettings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
				else if (!(NewChangedSettings & EApplicationSetting_RunAsUserPassword))
				{
					NewSettings.m_RunAsUserPassword.f_Clear();
					for (auto &pOtherApplication : mp_Applications)
					{
						auto &OtherSettings = pOtherApplication->m_Settings;
						if (OtherSettings.m_RunAsUser == NewSettings.m_RunAsUser && !OtherSettings.m_RunAsUserPassword.f_IsEmpty())
						{
							NewSettings.m_RunAsUserPassword = OtherSettings.m_RunAsUserPassword;
							break;
						}
					}

					if (NewSettings.m_RunAsUserPassword.f_IsEmpty())
						co_return DMibErrorInstance("The account of run-as user '{}' exists, but no application runs as it, so its password is unknown"_f << NewSettings.m_RunAsUser);
				}
			}
			else if (!NewSettings.m_RunAsUser.f_IsEmpty() && NewSettings.m_RunAsUserPassword.f_IsEmpty())
				NewSettings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
#endif

			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return DMibErrorInstance(fg_Format("Updating settings resulted in invalid settings: {}", Error));

			// A check of the settings, not of the files: a Local environment the operator adds while the update runs isn't checked again before the
			// files change owner, and fp_CheckLocalEnvironmentAccounts refuses its agent at the next start
			if (CStr AccountError = fp_CheckLocalEnvironmentApplicationAccount(Application.m_Name, NewSettings); !AccountError.f_IsEmpty())
				co_return DMibErrorInstance(fg_Format("Updating settings resulted in invalid settings: {}", AccountError));

			State.m_pNewSettings = fg_Construct(NewSettings);
		}

		State.m_bSetLastTried = true;
		Application.m_LastTriedInstalledVersion = State.m_VersionID;
		Application.m_LastTriedInstalledVersionInfo = VersionInfo;
		Application.m_LastTriedInstalledVersionError.f_Clear();

		State.m_fUpdateVersionInfo = [pApplication = State.m_pApplication, VersionID = State.m_VersionID, VersionInfo]
			{
				pApplication->m_LastInstalledVersion = VersionID;
				pApplication->m_LastInstalledVersionInfo = VersionInfo;
			}
		;

		co_await fp_UpdateApplicationJson(State.m_pApplication);
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_Unpack(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Extracting application to temporary directory");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_Unpack, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		// Moving the application to another launch environment needs access to the environment, which automatic updates
		// have no caller for, and can need the files in another storage or with other owners
		if (State.m_pNewSettings && State.m_pNewSettings->m_LaunchEnvironment != State.m_pApplication->m_Settings.m_LaunchEnvironment)
		{
			co_return DMibErrorInstance
				(
					"The new version's settings change the launch environment to '{}'. Change it with --application-change-settings."_f << State.m_pNewSettings->m_LaunchEnvironment
				)
			;
		}

		bool bRemoteStorage = (bool)State.m_pRemoteStorageEnvironment;

		if (bRemoteStorage)
		{
			if (CStr Error = fp_CheckEnvironmentStagingStorage(*State.m_pRemoteStorageEnvironment); !Error.f_IsEmpty())
				co_return DMibErrorInstance(Error);
		}

		CStr TemporaryDirectoryRoot;
		// The environment storage on the host holds the unpacked files until they are staged into the environment
		if (bRemoteStorage)
			TemporaryDirectoryRoot = fp_GetEnvironmentStorageDirectory(*State.m_pRemoteStorageEnvironment) / ".AppStage" / State.m_pApplication->m_Name / "Unpack";
		else
			TemporaryDirectoryRoot = State.m_pApplication->f_GetDirectory() / "TempVersion";
		CStr BuggyTemporaryDirectoryRoot = State.m_pApplication->f_GetDirectory() / "{}";
		CStr TemporaryDirectory = TemporaryDirectoryRoot / fg_FastRandomID();
		State.m_TempraryPath = TemporaryDirectory;
		State.m_TemporaryDirectoryCleanup = g_BlockingActorSubscription / [TemporaryDirectoryRoot]
			{
				try
				{
					if (CFile::fs_FileExists(TemporaryDirectoryRoot))
						CFile::fs_DeleteDirectoryRecursive(TemporaryDirectoryRoot);
				}
				catch (CExceptionFile const &_Exception)
				{
					[[maybe_unused]] auto &Exception = _Exception;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up temp unpack: {}", Exception);
				}
			}
		;

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			State.m_Files = co_await
				(
					g_Dispatch(BlockingActorCheckout) /
					[
						=
						, Settings = State.m_pNewSettings ? *State.m_pNewSettings : State.m_pApplication->m_Settings
						, OutputDirectory = State.m_pApplication->f_GetDirectory()
						, ApplicationName = State.m_pApplication->m_Name
						, fOnInfo = State.m_fOnInfo
						, AllowSourceExist = State.m_AllowSourceExist
						, SourcePath = State.m_SourcePath
						, pUniqueUserGroup = mp_pUniqueUserGroup
						, RootDirectory = mp_State.m_RootDirectory
					]
					() mutable
					{
						// The environment agent resolves the guest users and groups and sets the file ownership
						if (bRemoteStorage)
						{
							Settings.m_RunAsUser = {};
							Settings.m_RunAsGroup = {};
						}
						else
							fsp_CreateApplicationUserGroup(Settings, fOnInfo, OutputDirectory / ".home", pUniqueUserGroup);

						if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
						{
							auto Files = CFile::fs_FindFiles(SourcePath + "/*");
							if (Files.f_GetLen() == 1 && (Files[0].f_EndsWith(".tar.gz") || Files[0].f_EndsWith(".tar.zst") || Files[0].f_EndsWith(".tar")))
								SourcePath = Files[0];
						}

						// Cleanup any old crash version
						try
						{
							if (CFile::fs_FileExists(TemporaryDirectoryRoot))
								CFile::fs_DeleteDirectoryRecursive(TemporaryDirectoryRoot);
							if (!bRemoteStorage && CFile::fs_FileExists(BuggyTemporaryDirectoryRoot))
								CFile::fs_DeleteDirectoryRecursive(BuggyTemporaryDirectoryRoot);
						}
						catch (CExceptionFile const &_Exception)
						{
							[[maybe_unused]] auto &Exception = _Exception;
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up old temp unpack: {}", Exception);
						}

						TCVector<CStr> Files;
						CStr Output = fsp_UnpackApplication
							(
								RootDirectory
								, SourcePath
								, TemporaryDirectory
								, ApplicationName
								, Settings
								, Files
								, AllowSourceExist
								, false
								, bRemoteStorage
								, pUniqueUserGroup
							)
						;
						if (!Output.f_IsEmpty())
							fOnInfo(Output);
						return Files;
					}
				)
			;
		}

		// Staging starts the environment and prepares the guest, which a dry run must not change
		if (!bRemoteStorage || State.m_bDryRun)
			co_return {};

		co_return co_await fp_StageApplicationInEnvironment(_pState, State.m_pRemoteStorageEnvironment);
	}

	TCFuture<void> CAppManagerActor::fp_StageApplicationInEnvironment(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState, TCSharedPointer<CEnvironment> _pEnvironment)
	{
		auto &State = *_pState;

		++State.m_pApplication->m_nGuestWaits;
		auto EndGuestWait = g_OnScopeExit / [pApplication = State.m_pApplication]
			{
				--pApplication->m_nGuestWaits;
			}
		;

		// The transfer below is outside, since a failed transfer destroys the stage send and the stage finish
		{
			auto OnResume = co_await fg_OnResume
				(
					[&]() -> CExceptionPointer
					{
						return State.f_CheckAbort();
					}
				)
			;

			State.m_fOnInfo("Transferring the application files into the environment");

			if (_pEnvironment->m_bStoppedManually)
				co_return DMibErrorInstance("Environment '{}' was stopped. Start it with --environment-start."_f << _pEnvironment->m_Name);

			co_await fp_EnsureEnvironmentStarted(_pEnvironment);

			// Creating the application accounts when the staging finishes can rely on the environment OS dependencies
			co_await (fp_WaitForEnvironmentAgentConfiguration(_pEnvironment) % "Failed to configure the environment");
		}

		if (!_pEnvironment->m_AgentInterface)
			co_return DMibErrorInstance("The environment agent is not connected");

		State.m_StageSend = fg_ConstructActor<CFileTransferSend>(fg_TempCopy(State.m_TempraryPath));

		auto SendFilesResult = co_await fg_TempCopy(State.m_StageSend)(&CFileTransferSend::f_SendFiles, CFileTransferSend::CSendFilesOptions()).f_Wrap();
		if (!SendFilesResult)
		{
			if (State.m_StageSend)
				co_await fg_Move(State.m_StageSend).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage send");

			co_return SendFilesResult.f_GetException();
		}

		auto &Settings = State.m_pNewSettings ? *State.m_pNewSettings : State.m_pApplication->m_Settings;

		CAppManagerEnvironmentInterface::CApplicationStage Stage;
		Stage.m_Name = State.m_pApplication->m_Name;
		Stage.m_RunAsUser = Settings.m_RunAsUser;
		Stage.m_RunAsGroup = Settings.m_RunAsGroup;
		Stage.m_bRunAsUserHasShell = Settings.m_bRunAsUserHasShell;
		Stage.m_FilesGenerator = CFileTransferSendDownloadFile::fs_TranslateGenerator<CAppManagerEnvironmentInterface::CStageFile>(fg_Move(SendFilesResult->m_FilesGenerator));
		Stage.m_FilesGenerator->f_SetSubscription(fg_Move(SendFilesResult->m_Subscription));

		auto TransferResultFuture = fg_Move(SendFilesResult->m_Result);

		// The agent can disconnect while the transfer is prepared
		TCAsyncResult<CAppManagerEnvironmentInterface::CApplicationStageResult> StageResult;
		if (_pEnvironment->m_AgentInterface)
		{
			StageResult = co_await _pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_StageApplicationFiles)
				(fg_Move(Stage))
				.f_Timeout(600.0, "Timed out starting to stage the application files in the environment (10 minutes)")
				.f_Wrap()
			;
		}
		else
			StageResult.f_SetException(DMibErrorInstance("The environment agent disconnected before the application files were staged"));

		if (!StageResult)
		{
			if (State.m_StageSend)
				co_await fg_Move(State.m_StageSend).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage send");

			co_await fg_Move(TransferResultFuture).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "The aborted application file transfer failed");

			co_return StageResult.f_GetException();
		}

		State.m_fStageFinish = fg_Move(StageResult->m_fFinish);

		auto TransferResult = co_await fg_Move(TransferResultFuture).f_Timeout(60.0 * 60.0, "Timed out transferring the application files into the environment (1 hour)").f_Wrap();

		if (State.m_StageSend)
			co_await fg_Move(State.m_StageSend).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage send");

		if (!TransferResult || State.f_CheckAbort())
		{
			if (!State.m_fStageFinish.f_IsEmpty())
				co_await fg_Move(State.m_fStageFinish).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage finish functor");

			if (auto pException = State.f_CheckAbort())
				co_return pException;

			co_return TransferResult.f_GetException();
		}

		if (State.m_fStageFinish.f_IsEmpty() || !State.m_fStageFinish.f_GetFunctor())
			co_return DMibErrorInstance("The application staging was aborted");

		// The finish waits for the transfer and for the agent to give the files to the application account. Destroying the functor below aborts a
		// stage whose transfer still runs when the finish times out
		auto FinishResult = co_await State.m_fStageFinish().f_Timeout(60.0 * 60.0, "Timed out finishing the application stage in the environment (1 hour)").f_Wrap();

		if (!State.m_fStageFinish.f_IsEmpty())
			co_await fg_Move(State.m_fStageFinish).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage finish functor");

		if (!FinishResult)
			co_return FinishResult.f_GetException();

		State.m_AgentStageID = StageResult->m_StageID;

		// The pre update script gets the stage directory inside the environment, not the host directory the package was unpacked to
		State.m_TempraryPath = StageResult->m_StageDirectory;

		// Once staging succeeded, an update that unwinds before the apply discards the stage if the agent is still reachable
		{
			auto pEnvironment = _pEnvironment;
			CStr Name = State.m_pApplication->m_Name;
			CStr StageID = State.m_AgentStageID;
			State.m_AgentStageCleanup = g_ActorSubscription / [pEnvironment, Name, StageID]() -> TCFuture<void>
				{
					if (pEnvironment->m_bDeleted || !pEnvironment->m_AgentInterface)
						co_return {};

					co_await pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_DiscardApplicationStage)
						(Name, StageID)
						.f_Timeout(120.0, "Timed out discarding the application stage")
						.f_Wrap()
						> fg_LogError("Malterlib/Cloud/AppManager", "Failed to discard the application stage in the environment")
					;

					co_return {};
				}
			;
		}

		if (State.m_TemporaryDirectoryCleanup)
			co_await State.m_TemporaryDirectoryCleanup->f_Destroy();
		if (State.m_DownloadDirectoryCleanup)
			co_await State.m_DownloadDirectoryCleanup->f_Destroy();
		State.m_DownloadDirectoryCleanup.f_Clear();
		State.m_TemporaryDirectoryCleanup.f_Clear();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_StopOldApp(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Stopping old application");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_StopOldApp, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		TCAsyncResult<uint32> ExitStatus = co_await State.m_pApplication->f_Stop(EStopFlag_PreventLaunchUpdate).f_Wrap();

		NStr::CStr Error = fp_GetApplicationStopErrors(ExitStatus, State.m_pApplication->m_Name);

		if (!Error.f_IsEmpty())
			State.m_Auditor.f_Warning(Error);

		if (!ExitStatus)
			co_return DMibErrorInstance("Failed to exit old application, aborting update");

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_PreUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Pre update");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PreUpdateScript, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		co_await fp_RunUpdateScript
			(
				State.m_pApplication
				, EUpdateScript_PreUpdate
				, State.m_TempraryPath
				, State.m_VersionID
				, State.m_pVersionInfo.f_Get()
				, State.m_LastInstalledVersion
				, State.m_LastInstalledVersionInfo
				, State.m_pStopwatch->f_GetTime()
				, true
			)
		;
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Updating application files");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_UpdateApplicationFiles, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		if (State.m_pRemoteStorageEnvironment)
		{
			auto pEnvironment = State.m_pRemoteStorageEnvironment;

			++State.m_pApplication->m_nGuestWaits;
			auto EndGuestWait = g_OnScopeExit / [pApplication = State.m_pApplication]
				{
					--pApplication->m_nGuestWaits;
				}
			;

			co_await fp_EnsureEnvironmentStarted(pEnvironment);

			if (auto pException = State.f_CheckAbort())
				co_return pException;

			if (!pEnvironment->m_AgentInterface)
				co_return DMibErrorInstance("The environment agent is not connected");

			auto &Settings = State.m_pNewSettings ? *State.m_pNewSettings : State.m_pApplication->m_Settings;

			CAppManagerEnvironmentInterface::CApplicationApply Apply;
			Apply.m_Name = State.m_pApplication->m_Name;
			Apply.m_StageID = State.m_AgentStageID;
			Apply.m_Files = State.m_Files;
			Apply.m_OldFiles = State.m_pApplication->m_Files;
			Apply.m_RunAsUser = Settings.m_RunAsUser;
			Apply.m_RunAsGroup = Settings.m_RunAsGroup;
			Apply.m_bRunAsUserHasShell = Settings.m_bRunAsUserHasShell;

			// Absent extra info doesn't encode as JSON, and the agent skips an empty one; invalid nested values throw
			if (State.m_pVersionInfo && State.m_pVersionInfo->m_ExtraInfo.f_IsValid())
			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to encode the extra info of the version");
				Apply.m_VersionExtraInfo = State.m_pVersionInfo->m_ExtraInfo.f_ToString(nullptr);
			}

			// The apply deletes the old files before it moves the new ones into place, so a stop in the middle leaves the application without
			// complete files
			++pEnvironment->m_nFileApplies;
			auto EndFileApply = g_OnScopeExit / [pEnvironment]
				{
					--pEnvironment->m_nFileApplies;
				}
			;

			co_await pEnvironment->m_AgentInterface.f_CallActor(&CAppManagerEnvironmentInterface::f_ApplyApplicationFiles)
				(fg_Move(Apply))
				.f_Timeout(60.0 * 60.0, "Timed out applying the application files in the environment (1 hour)")
			;

			// The apply moved the staged files and deleted the stage directory, so nothing is left to discard
			State.m_AgentStageCleanup.f_Clear();

			State.m_pApplication->m_Files = State.m_Files;

			co_return {};
		}

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			State.m_Files = co_await
				(
					g_Dispatch(BlockingActorCheckout)
					/ [OutputDirectory = State.m_pApplication->f_GetDirectory(), TemporaryDirectory = State.m_TempraryPath, OldFiles = State.m_pApplication->m_Files, Files = State.m_Files]
					{
						return fg_AppManager_ReplaceApplicationFiles(OutputDirectory, TemporaryDirectory, OldFiles, Files);
					}
				)
			;

			// The next update deletes the old files by this list, also when changing the owners fails
			State.m_pApplication->m_Files = State.m_Files;

			State.m_pApplication->m_LowPortExecutablesToGrant = co_await
				(
					g_Dispatch(BlockingActorCheckout) /
					[
						OutputDirectory = State.m_pApplication->f_GetDirectory()
						, Files = State.m_Files
						, pApplication = State.m_pApplication
						, Settings = State.m_pNewSettings ? *State.m_pNewSettings : State.m_pApplication->m_Settings
						, pUniqueUserGroup = mp_pUniqueUserGroup
						, fOnInfo = State.m_fOnInfo
					]
					{
						return fsp_UpdateApplicationFilePermissions(OutputDirectory, pApplication, Settings, Files, pUniqueUserGroup, fOnInfo, true);
					}
				)
			;
		}

		if (State.m_TemporaryDirectoryCleanup)
			co_await State.m_TemporaryDirectoryCleanup->f_Destroy();
		if (State.m_DownloadDirectoryCleanup)
			co_await State.m_DownloadDirectoryCleanup->f_Destroy();
		State.m_DownloadDirectoryCleanup.f_Clear();
		State.m_TemporaryDirectoryCleanup.f_Clear();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_SaveApplicationState(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		// The files of the new version are in place, with the owners of the new settings, so the settings and the version that match them are
		// saved also when the update is cancelled or its notification fails. The save is outside, so a deletion while it runs doesn't replace the
		// result of the notification
		TCAsyncResult<void> EventResult;
		{
			auto OnResume = co_await fg_OnResume
				(
					[&]() -> CExceptionPointer
					{
						if (State.m_pApplication->m_bDeleted)
							return DMibErrorInstance("Application has been deleted, aborting");

						return {};
					}
				)
			;

			State.m_fOnInfo("Saving application state");

			// A cancelled update doesn't wait for the subscribers of the notification, since shutdown waits for it
			if (auto pException = State.f_CheckAbort())
				EventResult.f_SetException(fg_Move(pException));
			else
				EventResult = co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_SaveApplicationState, {}).f_Wrap();
		}

		bool bSettingsChange = false;
		if (State.m_pNewSettings)
		{
			State.m_pApplication->m_Settings = *State.m_pNewSettings;
			bSettingsChange = true;
		}

		if (State.m_fUpdateVersionInfo)
		{
			State.m_fUpdateVersionInfo();
			bSettingsChange = true;

			// A package from file can be for a platform that no installed version had, which the version manager subscriptions filter out
			if (mp_KnownPlatforms(State.m_pApplication->m_LastInstalledVersion.m_Platform).f_WasCreated())
				fp_VersionManagerResubscribeAll() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to resubscribe to version managers");
		}

		if (bSettingsChange)
			fp_SendAppChange_AddedOrChanged(*State.m_pApplication);

		// An update that stops here skips its post update script, so the application stays blocked from launching, also after a restart of the
		// AppManager, like an update that stops before its files are replaced
		if (EventResult && !State.f_CheckAbort())
			State.m_pApplication->m_bPreventLaunch_Update = false;

		co_await fp_UpdateApplicationJson(State.m_pApplication);

		if (!EventResult)
			co_return EventResult.f_GetException();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_PostUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PostUpdateScript, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		co_await fp_RunUpdateScript
			(
				State.m_pApplication
				, EUpdateScript_PostUpdate
				, CStr{}
				, State.m_VersionID
				, State.m_pVersionInfo.f_Get()
				, State.m_LastInstalledVersion
				, State.m_LastInstalledVersionInfo
				, State.m_pStopwatch->f_GetTime()
				, true
			)
		;
		co_return {};
	}

	TCFuture<bool> CAppManagerActor::fp_UpdateApplication_StartNewApp(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Launching updated application");
		State.m_pApplication->m_bJustUpdated = true;
		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_StartNewApp, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		CStr Message;
		CAppManagerInterface::EStatusSeverity Severity;

		State.m_pApplication->m_bPreventLaunch_DelayAfterFailure = false;

		bool bNeedsEncryption = false;
		if (!State.m_pApplication->f_DependenciesSatisfied(Message, Severity, bNeedsEncryption))
		{
			if (State.m_VersionID.f_IsValid())
				State.m_fOnInfo(fg_Format("Application was successfully updated to version {}. Launch skipped because of missing dependency: {}", State.m_VersionID, Message));
			else
				State.m_fOnInfo(fg_Format("Application was successfully updated. Launch skipped because of missing dependency: {}", Message));

			fp_SetAppLaunchStatus(State.m_pApplication, Message, Severity);

			State.m_pApplication->m_bAutoStart = true;

			co_return false;
		}

		CAppLaunchResult AppLaunchResult = co_await fp_LaunchApp(State.m_pApplication, false);

		if (!AppLaunchResult.m_StartupError.f_IsEmpty())
		{
			fp_RunUpdateScript
				(
					State.m_pApplication
					, EUpdateScript_OnError
					, "Error starting updated application: {}"_f << AppLaunchResult.m_StartupError
					, State.m_VersionID
					, State.m_pVersionInfo.f_Get()
					, State.m_LastInstalledVersion
					, State.m_LastInstalledVersionInfo
					, State.m_pStopwatch->f_GetTime()
					, false
				)
				> fg_LogError("Malterlib/Cloud/AppManager", "Error script failed")
			;
			if (!AppLaunchResult.m_bQuitManager)
				co_return DMibErrorInstance(AppLaunchResult.m_StartupError);
		}

		if (State.m_VersionID.f_IsValid())
			State.m_fOnInfo(fg_Format("Application was successfully updated to version {}", State.m_VersionID));
		else
			State.m_fOnInfo("Application was successfully updated");

		co_return AppLaunchResult.m_bQuitManager;
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_PostLaunch(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		auto &State = *_pState;

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PostLaunch, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		TCAsyncResult<void> ScriptResult = co_await fp_RunUpdateScript
			(
				State.m_pApplication
				, EUpdateScript_PostLaunch
				, CStr{}
				, State.m_VersionID
				, State.m_pVersionInfo.f_Get()
				, State.m_LastInstalledVersion
				, State.m_LastInstalledVersionInfo
				, State.m_pStopwatch->f_GetTime()
				, true
			)
			.f_Wrap()
		;

		if (!ScriptResult)
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Post launch script failed: {}", ScriptResult.f_GetExceptionStr());

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_Finished, {}).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed waiting for finished event");

		fp_RestartEnvironmentsForAgentApplication(_pState->m_pApplication->m_Name);

		co_return {};
	}

	void CAppManagerActor::fp_RestartEnvironmentsForAgentApplication(CStr const &_ApplicationName)
	{
		for (auto &pEnvironment : mp_Environments)
		{
			auto pAgentApplication = fp_FindEnvironmentAgentApplication(*pEnvironment);
			if (!pAgentApplication || pAgentApplication->m_Name != _ApplicationName)
				continue;

			if (!pEnvironment->f_IsStarted() && !pEnvironment->m_bStarting)
				continue;

			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Restarting environment '{}' after update of agent application '{}'", pEnvironment->m_Name, _ApplicationName);

			fp_RestartEnvironmentWhenIdle(pEnvironment) > fg_LogError("Malterlib/Cloud/AppManager", "Failed to restart environment after agent application update");
		}
	}
}
