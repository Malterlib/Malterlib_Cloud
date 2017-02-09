// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<void> CAppManagerActor::fp_UpdateApplicationRunProcess(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		TCContinuation<void> Continuation;
		
		fp_OnUpdateEvent(_pState->m_pApplication, CAppManagerInterface::EUpdateStage_None, _pState->m_VersionID, {}) > Continuation / [=]
		{
			fp_OnUpdateEvent(_pState->m_pApplication, CAppManagerInterface::EUpdateStage_ChangeEncryption, _pState->m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
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
												fp_UpdateApplication_StartNewApp(_pState) > Continuation % "Failed to start new app. Will retry periodically" / [=]
												{
													fp_UpdateApplication_PostLaunch(_pState) > Continuation % "Failed post launch" / [=]
													{
														Continuation.f_SetResult();
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
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_DownloadVersion(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		if (!State.m_SourcePath.f_IsEmpty())
			return fg_Explicit(); // Already specified

		State.m_fOnInfo(fg_Format("Downloading version '{}' from version managers", State.m_VersionID));
		
		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_DownloadVersion, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
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
							catch (CExceptionFile const &_Exception)
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
						
						if (!State.m_bDryRun)
						{
							if (State.m_bUpdateSettings)
							{
								CApplicationSettings NewSettings = Application.m_Settings;
								EApplicationSetting NewChangedSettings = EApplicationSetting_None;
								NewSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
								
								CStr Error;
								if (!NewSettings.f_Validate(Error))
								{
									Continuation.f_SetException(DMibErrorInstance(fg_Format("Updating settings resulted in invalid settings: {}", Error)));
									return;
								}
								State.m_pNewSettings = fg_Construct(NewSettings);
							}
							
							Application.m_LastTriedInstalledVersion = State.m_VersionID;
							Application.m_LastTriedInstalledVersionInfo = VersionInfo;
							
							State.m_fUpdateVersionInfo = [pApplication = State.m_pApplication, VersionID = State.m_VersionID, VersionInfo, this]
								{
									pApplication->m_LastInstalledVersion = VersionID;
									pApplication->m_LastInstalledVersionInfo = VersionInfo;
									fp_RemoteAppInfoChanged(pApplication);
								}
							;
						}
						Continuation.f_SetResult();
					}
				;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_Unpack(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Extracting application to temporary directory");
		
		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_Unpack, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
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
							catch (CExceptionFile const &_Exception)
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
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_StopOldApp(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Stopping old application");
		
		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_StopOldApp, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
				State.m_pApplication->f_Stop(false) > [=](TCAsyncResult<uint32> &&_ExitStatus)
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
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PreUpdate(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Pre update");
		
		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_PreUpdateScript, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));

				fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PreUpdate, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()) > Continuation;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_UpdateApplicationFiles(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Updating application files");

		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_UpdateApplicationFiles, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
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
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_SaveApplicationState(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Saving application state");

		TCContinuation<void> Continuation; 
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_SaveApplicationState, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
				if (State.m_pNewSettings)
					State.m_pApplication->m_Settings = *State.m_pNewSettings;
				
				if (State.m_fUpdateVersionInfo)
					State.m_fUpdateVersionInfo();

				fp_UpdateApplicationJSON(State.m_pApplication) > Continuation;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PostUpdate(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState;
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		TCContinuation<void> Continuation;
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_PostUpdateScript, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
				fp_RunUpdateScript(State.m_pApplication, EUpdateScript_PostUpdate, CStr{}, State.m_VersionID, State.m_pVersionInfo.f_Get(), State.m_pClock->f_GetTime()) > Continuation;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_StartNewApp(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		State.m_fOnInfo("Launching updated application");
		State.m_pApplication->m_bJustUpdated = true;

		TCContinuation<void> Continuation; 
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_StartNewApp, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
				fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, State.m_pApplication, false)
					> Continuation / [=]
					{
						auto &State = *_pState; 
						if (State.m_VersionID.f_IsValid())
							State.m_fOnInfo(fg_Format("Application was successfully updated to version {}", State.m_VersionID));
						else
							State.m_fOnInfo("Application was successfully updated");
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication_PostLaunch(TCSharedPointer<CUpdateApplicationState> const &_pState)
	{
		auto &State = *_pState; 
		if (State.m_pApplication->m_bDeleted)
			return DMibErrorInstance("Application has been deleted, aborting");
		
		TCContinuation<void> Continuation; 
		fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_PostLaunch, State.m_VersionID, {}) > Continuation / [=]
			{
				auto &State = *_pState; 
				if (State.m_pApplication->m_bDeleted)
					return Continuation.f_SetException(DMibErrorInstance("Application has been deleted, aborting"));
				
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
						fp_OnUpdateEvent(State.m_pApplication, CAppManagerInterface::EUpdateStage_Finished, State.m_VersionID, {}) > fg_DiscardResult();
						
						Continuation.f_SetResult();
					}
				;
			}
		;
		return Continuation;
	}
}
