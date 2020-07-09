// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	void CAppManagerActor::fp_StartPendingSelfUpdateReporting
		(
			CStr const &_Name
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CTime const &_VersionTime
			, uint32 _VersionRetrySequence
		)
	{
		auto *pApplication = mp_Applications.f_FindEqual(_Name);
		if (!pApplication)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Could not find pending self update application: {}", _Name);
			return;
		}

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Starting pending self update reporting");

		mp_bPendingSelfUpdateInProgress = true;
		auto pCleanupAutoUpdate = g_OnScopeExitActor > [this]
			{
				mp_bPendingSelfUpdateInProgress = false;
				fp_AutoUpdate_Update();
			}
		;

		TCSharedPointerSupportWeak<CUpdateApplicationState> pUpdateState = fg_Construct();
		pUpdateState->m_pApplication = *pApplication;
		pUpdateState->m_VersionID = _VersionID;
		pUpdateState->m_VersionTime = _VersionTime;
		pUpdateState->m_VersionRetrySequence = _VersionRetrySequence;
		pUpdateState->m_fOnInfo = [Name = (*pApplication)->m_Name](CStr const &_Info)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pending self update '{}': {}", Name, _Info);
			}
		;

		mp_InitialStartupResult.f_Future() > [this, pUpdateState, pCleanupAutoUpdate](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					CStr Error = fg_Format("Pending self update failed: {}", _Result.f_GetExceptionStr());
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "{}", Error);
					fp_OnUpdateEvent(pUpdateState, EUpdateStage::EUpdateStage_Failed, Error) > fg_DiscardResult();
					return;
				}

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Pending self update finished");
				fp_OnUpdateEvent(pUpdateState, EUpdateStage::EUpdateStage_Finished, {}) > fg_DiscardResult();
			}
		;
	}

	TCFuture<void> CAppManagerActor::fp_UpdateApplication_DeferToNextRestart
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
			, TCSharedPointer<CCanDestroyTracker> _pCanDestroy
		)
	{
		auto &Application = *_pState->m_pApplication;

		mp_State.m_StateDatabase.m_Data["PendingSelfUpdateProcess"] =
			{
				"Name"_= Application.m_Name
				, "VersionID"_= _pState->m_VersionID.f_ToJSON()
				, "VersionTime"_= _pState->m_VersionTime
				, "VersionRetrySequence"_= _pState->m_VersionRetrySequence
			}
		;

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

		co_await (self(&CAppManagerActor::fp_ChangeEncryption, _pState->m_pApplication, EEncryptOperation_Open, false) % "Failed to open encryption");

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

		if (!State.m_SourcePath.f_IsEmpty())
			co_return {}; // Already specified

		State.m_fOnInfo(fg_Format("Downloading version '{}' from version managers", State.m_VersionID));

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_DownloadVersion, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		CStr DownloadDirectoryRoot = State.m_pApplication->f_GetDirectory() / "TempVersionDownload";
		CStr DownloadDirectory = DownloadDirectoryRoot / fg_RandomID();
		State.m_SourcePath = DownloadDirectory;
		State.m_AllowSourceExist[DownloadDirectoryRoot];

		State.m_DownloadDirectoryCleanup = g_ActorSubscription(mp_FileActor) / [DownloadDirectoryRoot]
			{
				try
				{
					if (CFile::fs_FileExists(DownloadDirectoryRoot))
						CFile::fs_DeleteDirectoryRecursive(DownloadDirectoryRoot);
				}
				catch (CExceptionFile const &_Exception)
				{
					(void)_Exception;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up version download: {}", _Exception);
				}
			}
		;

		CVersionManager::CVersionInformation VersionInfo = co_await
			self(&CAppManagerActor::fp_DownloadApplication, State.m_pApplication->m_Settings.m_VersionManagerApplication, State.m_VersionID, DownloadDirectory)
		;

		auto &Application = *State.m_pApplication;

		if (Application.m_bDeleted)
			co_return DMibErrorInstance("Application has been deleted, aborting");

		State.m_fOnInfo(fg_Format("Application downloaded, unpacking"));
		State.m_pVersionInfo = fg_Construct(VersionInfo);

		for (auto &Tag : State.m_RequiredTags)
		{
			if (!VersionInfo.m_Tags.f_FindEqual(Tag))
				co_return DMibErrorInstance(fg_Format("Missing required tag: '{}'", Tag));
		}

		if (State.m_bDryRun)
			co_return {};

		if (State.m_bUpdateSettings)
		{
			CApplicationSettings NewSettings = Application.m_Settings;
			EApplicationSetting NewChangedSettings = EApplicationSetting_None;

			try
			{
				NewSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
			}
			catch (CException const &_Exception)
			{
				co_return DMibErrorInstance(fg_Format("Failed to get settings from version info: {}", _Exception));
			}

			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return DMibErrorInstance(fg_Format("Updating settings resulted in invalid settings: {}", Error));

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

		co_await fp_UpdateApplicationJSON(State.m_pApplication);
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

		CStr TemporaryDirectoryRoot = State.m_pApplication->f_GetDirectory() / "TempVersion";
		CStr BuggyTemporaryDirectoryRoot = State.m_pApplication->f_GetDirectory() / "{}";
		CStr TemporaryDirectory = TemporaryDirectoryRoot / fg_RandomID();
		State.m_TempraryPath = TemporaryDirectory;
		State.m_TemporaryDirectoryCleanup = g_ActorSubscription(mp_FileActor) / [TemporaryDirectoryRoot]
			{
				try
				{
					if (CFile::fs_FileExists(TemporaryDirectoryRoot))
						CFile::fs_DeleteDirectoryRecursive(TemporaryDirectoryRoot);
				}
				catch (CExceptionFile const &_Exception)
				{
					(void)_Exception;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up temp unpack: {}", _Exception);
				}
			}
		;

		State.m_Files = co_await
			(
				g_Dispatch(mp_FileActor) /
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
					fsp_CreateApplicationUserGroup(Settings, fOnInfo, OutputDirectory / ".home", pUniqueUserGroup);


					if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
					{
						auto Files = CFile::fs_FindFiles(SourcePath + "/*");
						if (Files.f_GetLen() == 1 && Files[0].f_Right(7) == ".tar.gz")
							SourcePath = Files[0];
					}

					// Cleanup any old crash version
					try
					{
						if (CFile::fs_FileExists(TemporaryDirectoryRoot))
							CFile::fs_DeleteDirectoryRecursive(TemporaryDirectoryRoot);
						if (CFile::fs_FileExists(BuggyTemporaryDirectoryRoot))
							CFile::fs_DeleteDirectoryRecursive(BuggyTemporaryDirectoryRoot);
					}
					catch (CExceptionFile const &_Exception)
					{
						(void)_Exception;
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up old temp unpack: {}", _Exception);
					}

					TCVector<CStr> Files;
					CStr Output = fsp_UnpackApplication(RootDirectory, SourcePath, TemporaryDirectory, ApplicationName, Settings, Files, AllowSourceExist, false, pUniqueUserGroup);
					if (!Output.f_IsEmpty())
						fOnInfo(Output);
					return Files;
				}
			)
		;

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

		co_await fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PreUpdate, State.m_TempraryPath, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime());
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

		co_await
			(
				g_Dispatch(mp_FileActor) /
				[
					=
					, OutputDirectory = State.m_pApplication->f_GetDirectory()
					, TemporaryDirectory = State.m_TempraryPath
					, OldFiles = State.m_pApplication->m_Files
					, Files = State.m_Files
					, pApplication = State.m_pApplication
					, pUniqueUserGroup = mp_pUniqueUserGroup
				 	, fOnInfo = State.m_fOnInfo
				]
				{
					// 3. Delete old files
					for (auto &File : OldFiles)
					{
						CStr FullPath = fg_Format("{}/{}", OutputDirectory, File);
						if (CFile::fs_FileExists(FullPath, EFileAttrib_Directory))
						{
							try
							{
								// Allow only empty directories to be deleted, ignore error on non-empty ones.
								CFile::fs_DeleteDirectory(FullPath);
							}
							catch (CExceptionFile const &)
							{
							}
						}
						else if (CFile::fs_FileExists(FullPath, EFileAttrib_File | EFileAttrib_Link))
							CFile::fs_DeleteFile(FullPath);
					}

					// 4. Move files from temporary directory to final destination
					for (auto &File : Files)
					{
						CStr SourcePath = fg_Format("{}/{}", TemporaryDirectory, File);
						CStr DestinationPath = fg_Format("{}/{}", OutputDirectory, File);
						if (CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
						{
							CStr Directory = CFile::fs_GetPath(DestinationPath);
							CFile::fs_CreateDirectory(Directory);
							if (CFile::fs_FileExists(DestinationPath))
								CFile::fs_AtomicReplaceFile(SourcePath, DestinationPath);
							else
								CFile::fs_RenameFile(SourcePath, DestinationPath);
						}
					}

					fsp_UpdateApplicationFilePermissions(OutputDirectory, pApplication, Files, pUniqueUserGroup, fOnInfo);
				}
			)
		;

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

		if (auto pException = State.f_CheckAbort())
			co_return pException;

		State.m_fOnInfo("Saving application state");

		co_await fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_SaveApplicationState, {});

		if (auto pException = State.f_CheckAbort())
			co_return pException;

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
		}

		if (bSettingsChange)
			fp_SendAppChange_AddedOrChanged(*State.m_pApplication);

		State.m_pApplication->m_bPreventLaunch_Update = false;

		co_await fp_UpdateApplicationJSON(State.m_pApplication);
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

		co_await fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PostUpdate, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime());
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
		if (!State.m_pApplication->f_DependenciesSatisfied(Message, Severity))
		{
			if (State.m_VersionID.f_IsValid())
				State.m_fOnInfo(fg_Format("Application was successfully updated to version {}. Launch skipped because of missing dependency: {}", State.m_VersionID, Message));
			else
				State.m_fOnInfo(fg_Format("Application was successfully updated. Launch skipped because of missing dependency: {}", Message));

			fp_SetAppLaunchStatus(State.m_pApplication, Message, Severity);

			co_return false;
		}

		CAppLaunchResult AppLaunchResult = co_await fp_LaunchApp(State.m_pApplication, false);

		if (State.m_VersionID.f_IsValid())
			State.m_fOnInfo(fg_Format("Application was successfully updated to version {}", State.m_VersionID));
		else
			State.m_fOnInfo("Application was successfully updated");

		if (!AppLaunchResult.m_StartupError.f_IsEmpty())
		{
			fp_RunUpdateScript
				(
					State.m_pApplication
					, EUpdateScript_OnError
					, "Error starting updated application: {}"_f << AppLaunchResult.m_StartupError
					, State.m_VersionID
					, State.m_pVersionInfo.f_Get()
					, State.m_pClock->f_GetTime()
				)
				> fg_LogError("Malterlib/Cloud/AppManager", "Error script failed")
			;
		}

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

		TCAsyncResult<void> ScriptResult = co_await
			fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PostLaunch, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()).f_Wrap()
		;

		if (!ScriptResult)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Post launch script failed: {}"
					, ScriptResult.f_GetExceptionStr()
				)
			;
		}

		State.m_pInProgressScope.f_Clear();
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_Finished, {}) > fg_DiscardResult();

		co_return {};
	}
}
