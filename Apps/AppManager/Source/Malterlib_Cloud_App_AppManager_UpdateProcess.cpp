// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
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

		mp_InitialStartupResult > [this, pUpdateState, pCleanupAutoUpdate](TCAsyncResult<void> &&_Result)
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
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_DeferToNextRestart
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
			, TCSharedPointer<CCanDestroyTracker> const &_pCanDestroy
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
		
		TCContinuation<void> Continuation;

		fp_SaveStateDatabase() > Continuation % "Failed save state database" / [_pCanDestroy, Continuation]
			{
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplicationRunProcess(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		TCContinuation<void> Continuation;
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_None, {}) > Continuation / [=]
		{
			fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_SyncStart, {}) > Continuation / [=]
			{
				auto &State = *_pState;
				State.m_fOnInfo("Change encryption");
				fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_ChangeEncryption, {}) > Continuation / [=]
				{
					auto &State = *_pState;
					if (State.f_CheckAbort(Continuation))
						return;
					
					fp_ChangeEncryption(_pState->m_pApplication, EEncryptOperation_Open, false) > Continuation % "Failed to open encryption" / [=]
					{
						_pState->m_bUnencrypted = true;
						fp_UpdateApplication_DownloadVersion(_pState) > Continuation % "Failed to download version" / [=]
						{
							fp_UpdateApplication_Unpack(_pState) > Continuation % "Failed to unpack application" / [=]
							{
								if (_pState->m_bDryRun)
								{
									_pState->m_fOnInfo("Skipping stop, update and restart because of dry run");
									Continuation.f_SetResult();
									return;
								}
								fp_UpdateApplication_StopOldApp(_pState) > Continuation % "Failed to stop old app" / [=]
								{
									fp_UpdateApplication_PreUpdate(_pState) > Continuation % "Failed pre update app" / [=]
									{
										fp_UpdateApplication_UpdateApplicationFiles(_pState) > Continuation % "Failed to update application files" / [=]
										{
											fp_UpdateApplication_SaveApplicationState(_pState) > Continuation % "Failed to save application state" / [=]
											{
												fp_UpdateApplication_PostUpdate(_pState) > Continuation % "Failed post update" / [=]
												{
													auto pCanDestroy = mp_pCanDestroy;
													if (!pCanDestroy)
													{
														Continuation.f_SetException(DErrorInstance("Application stopped"));
														return;
													}

													fp_UpdateApplication_StartNewApp(_pState) > Continuation % "Failed to start new app. Will retry periodically" / [=](bool _bQuitManager)
													{
														auto &Application = *_pState->m_pApplication;
														if (!Application.m_bDeleted && _bQuitManager)
															fp_UpdateApplication_DeferToNextRestart(_pState, pCanDestroy) > Continuation;
														else
															fp_UpdateApplication_PostLaunch(_pState) > Continuation % "Failed post launch";
													};
												};
											};
										};
									};
								};
							};
						};
					};
				};
			};
		};
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_DownloadVersion(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		
		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		if (!State.m_SourcePath.f_IsEmpty())
			return fg_Explicit(); // Already specified

		State.m_fOnInfo(fg_Format("Downloading version '{}' from version managers", State.m_VersionID));
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_DownloadVersion, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				CStr DownloadDirectory = fg_Format("{}/TempVersionDownload", State.m_pApplication->f_GetDirectory());
				State.m_SourcePath = DownloadDirectory;
				State.m_AllowSourceExist[DownloadDirectory];
				
				State.m_pDownloadDirectoryCleanup = fg_OnScopeExitActor
					(
						mp_FileActor
						,  [DownloadDirectory]
						{
							try
							{
								if (CFile::fs_FileExists(DownloadDirectory))
									CFile::fs_DeleteDirectoryRecursive(DownloadDirectory);
							}
							catch (CExceptionFile const &)
							{
							}
						}
					)
				;

				self(&CAppManagerActor::fp_DownloadApplication, State.m_pApplication->m_Settings.m_VersionManagerApplication, State.m_VersionID, DownloadDirectory) 
					> Continuation / [=](CVersionManager::CVersionInformation &&_VersionInfo)
					{
						auto &State = *_pState;
						auto &Application = *State.m_pApplication; 

						if (Application.m_bDeleted)
						{
							Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
							return;
						}

						State.m_fOnInfo(fg_Format("Application downloaded, unpacking"));

						auto &VersionInfo = _VersionInfo;

						State.m_pVersionInfo = fg_Construct(VersionInfo);

						for (auto &Tag : State.m_RequiredTags)
						{
							if (!VersionInfo.m_Tags.f_FindEqual(Tag))
							{
								Continuation.f_SetException(DMibErrorInstance(fg_Format("Missing required tag: '{}'", Tag)));
								return;
							}
						}

						if (State.m_bDryRun)
						{
							Continuation.f_SetResult();
							return;
						}

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
								Continuation.f_SetException(DMibErrorInstance(fg_Format("Failed to get settings from version info: {}", _Exception)));
								return;
							}
							
							CStr Error;
							if (!NewSettings.f_Validate(Error))
							{
								Continuation.f_SetException(DMibErrorInstance(fg_Format("Updating settings resulted in invalid settings: {}", Error)));
								return;
							}
							State.m_pNewSettings = fg_Construct(NewSettings);
						}

						State.m_bSetLastTried = true;
						Application.m_LastTriedInstalledVersion = State.m_VersionID;
						Application.m_LastTriedInstalledVersionInfo = VersionInfo;
						
						State.m_fUpdateVersionInfo = [pApplication = State.m_pApplication, VersionID = State.m_VersionID, VersionInfo, this]
							{
								pApplication->m_LastInstalledVersion = VersionID;
								pApplication->m_LastInstalledVersionInfo = VersionInfo;
							}
						;

						fp_UpdateApplicationJSON(State.m_pApplication) > Continuation;
					}
				;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_Unpack(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;

		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		State.m_fOnInfo("Extracting application to temporary directory");
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_Unpack, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				CStr TemporaryDirectory = fg_Format("{}/TempVersion", State.m_pApplication->f_GetDirectory());
				
				State.m_pTemporaryDirectoryCleanup = fg_OnScopeExitActor
					(
						mp_FileActor
						,  [TemporaryDirectory]
						{
							try
							{
								if (CFile::fs_FileExists(TemporaryDirectory))
									CFile::fs_DeleteDirectoryRecursive(TemporaryDirectory);
							}
							catch (CExceptionFile const &)
							{
							}
						}
					)
				;
				g_Dispatch(mp_FileActor) > 
					[
						=
						, Settings = State.m_pNewSettings ? *State.m_pNewSettings : State.m_pApplication->m_Settings
						, OutputDirectory = State.m_pApplication->f_GetDirectory()
						, ApplicationName = State.m_pApplication->m_Name
						, fOnInfo = State.m_fOnInfo
						, AllowSourceExist = State.m_AllowSourceExist
						, SourcePath = State.m_SourcePath 
					]
					() mutable
					{
						fsp_CreateApplicationUserGroup(Settings, fOnInfo, OutputDirectory);
						
						
						if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
						{
							auto Files = CFile::fs_FindFiles(SourcePath + "/*");
							if (Files.f_GetLen() == 1 && Files[0].f_Right(7) == ".tar.gz")
								SourcePath = Files[0];
						}
						
						// Cleanup any old crash version
						if (CFile::fs_FileExists(TemporaryDirectory))
							CFile::fs_DeleteDirectoryRecursive(TemporaryDirectory);
						
						TCVector<CStr> Files;
						CStr Output = fsp_UnpackApplication(SourcePath, TemporaryDirectory, ApplicationName, Settings, Files, AllowSourceExist, false);
						if (!Output.f_IsEmpty())
							fOnInfo(Output);
						return Files;
					}
					> Continuation / [=](TCVector<CStr> &&_Files)
					{
						auto &State = *_pState;
						State.m_Files = fg_Move(_Files);
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_StopOldApp(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 

		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Stopping old application");
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_StopOldApp, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				State.m_pApplication->f_Stop(EStopFlag_PreventLaunchUpdate) > [=](TCAsyncResult<uint32> &&_ExitStatus)
					{
						auto &State = *_pState; 
						NStr::CStr Error = fp_GetApplicationStopErrors(_ExitStatus, State.m_pApplication->m_Name);

						if (!Error.f_IsEmpty())
							State.m_Auditor.f_Warning(Error);
						
						if (!_ExitStatus)
						{
							Continuation.f_SetException(DMibErrorInstance("Failed to exit old application, aborting update"));
							return;
						}
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PreUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		
		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		State.m_fOnInfo("Pre update");
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PreUpdateScript, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;

				fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PreUpdate, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()) > Continuation;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 

		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		State.m_fOnInfo("Updating application files");

		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_UpdateApplicationFiles, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				g_Dispatch(mp_FileActor) >
					[
						=
						, OutputDirectory = State.m_pApplication->f_GetDirectory()
						, TemporaryDirectory = fg_Format("{}/TempVersion", State.m_pApplication->f_GetDirectory())
						, OldFiles = State.m_pApplication->m_Files
						, Files = State.m_Files
						, pApplication = State.m_pApplication
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
								CFile::fs_RenameFile(SourcePath, DestinationPath);
							}
						}

						fsp_UpdateApplicationFiles(OutputDirectory, pApplication, Files);
					}
					> Continuation / [=]
					{
						auto &State = *_pState; 
						State.m_pDownloadDirectoryCleanup.f_Clear();
						State.m_pTemporaryDirectoryCleanup.f_Clear();
						
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_SaveApplicationState(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;

		TCContinuation<void> Continuation; 
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		State.m_fOnInfo("Saving application state");

		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_SaveApplicationState, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				if (State.m_pNewSettings)
					State.m_pApplication->m_Settings = *State.m_pNewSettings;
				
				if (State.m_fUpdateVersionInfo)
					State.m_fUpdateVersionInfo();
				
				State.m_pApplication->m_bPreventLaunch_Update = false;

				fp_UpdateApplicationJSON(State.m_pApplication) > Continuation;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PostUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;

		TCContinuation<void> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PostUpdateScript, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PostUpdate, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()) > Continuation;
			}
		;
		return Continuation;
	}
	
	TCContinuation<bool> CAppManagerActor::fp_UpdateApplication_StartNewApp(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 

		TCContinuation<bool> Continuation;
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		State.m_fOnInfo("Launching updated application");
		State.m_pApplication->m_bJustUpdated = true;
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_StartNewApp, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				CStr Message;
				if (!State.m_pApplication->f_DependenciesSatisfied(Message))
				{
					if (State.m_VersionID.f_IsValid())
						State.m_fOnInfo(fg_Format("Application was successfully updated to version {}. Launch skipped because of missing dependency: {}", State.m_VersionID, Message));
					else
						State.m_fOnInfo(fg_Format("Application was successfully updated. Launch skipped because of missing dependency: {}", Message));

					Continuation.f_SetResult(false);
					return;
				}

				fp_LaunchApp(State.m_pApplication, false)
					> Continuation / [=](bool _bQuitManager)
					{
						auto &State = *_pState; 
						if (State.m_VersionID.f_IsValid())
							State.m_fOnInfo(fg_Format("Application was successfully updated to version {}", State.m_VersionID));
						else
							State.m_fOnInfo("Application was successfully updated");
						Continuation.f_SetResult(_bQuitManager);
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PostLaunch(TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 

		TCContinuation<void> Continuation; 
		if (State.f_CheckAbort(Continuation))
			return Continuation;
		
		fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_PostLaunch, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.f_CheckAbort(Continuation))
					return;
				
				fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PostLaunch, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()) 
					> [=](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppManager
									, Warning
									, "Post launch script failed: {}"
									, _Result.f_GetExceptionStr()
								)
							;
						}
						
						auto &State = *_pState;
						State.m_pInProgressScope.f_Clear();
						fp_OnUpdateEvent(_pState, EUpdateStage::EUpdateStage_Finished, {}) > fg_DiscardResult();
						
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
}
