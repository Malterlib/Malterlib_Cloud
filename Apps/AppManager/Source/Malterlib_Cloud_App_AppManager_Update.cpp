// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CStr const &CAppManagerActor::CUpdateScripts::f_GetScript(EUpdateScript _Script) const
	{
		switch (_Script)
		{
		case EUpdateScript_PreUpdate:
			return m_PreUpdate;
		case EUpdateScript_PostUpdate:
			return m_PostUpdate;
		case EUpdateScript_PostLaunch:
			return m_PostLaunch;
		case EUpdateScript_OnError:
			return m_OnError;
		}
		DMibNeverGetHere;
		return m_PreUpdate;
	}
	
	CStr CAppManagerActor::CUpdateScripts::f_GetName(EUpdateScript _Script) const
	{
		switch (_Script)
		{
		case EUpdateScript_PreUpdate:
			return "PreUpdate";
		case EUpdateScript_PostUpdate:
			return "PostUpdate";
		case EUpdateScript_PostLaunch:
			return "PostLaunch";
		case EUpdateScript_OnError:
			return "OnError";
		}
		DMibNeverGetHere;
		return "Unknown";
	}

	TCContinuation<bool> CAppManagerActor::fp_RunUpdateScript
		(
			TCSharedPointer<CApplication> const &_pApplication
			, EUpdateScript _Script
			, CStr const &_Param
			, CVersionManager::CVersionIDAndPlatform const &_VersionID
			, CVersionManager::CVersionInformation *_pVersionInformation
			, fp64 _TimeSinceUpdateStart
		)
	{
		CStr Script = _pApplication->m_Settings.m_UpdateScripts.f_GetScript(_Script);
		if (Script.f_IsEmpty())
			return fg_Explicit(false);
		
		struct CState
		{
			TCContinuation<bool> m_Continuation;
			TCActor<CProcessLaunchActor> m_LaunchActor;
			CActorSubscription m_LaunchSubscription;
			CStr m_ErrorOutput;
			CStr m_StdOutput;
			CStr m_AllOutput;
			
			bool m_bReplied = false;
			void f_Replied()
			{
				m_bReplied = true;
				m_LaunchActor->f_Destroy();
			}
		};
		
		CStr Description = fg_Format("{}/{}", _pApplication->m_Name, _pApplication->m_Settings.m_UpdateScripts.f_GetName(_Script));
		
		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_LaunchActor = fg_ConstructActor<CProcessLaunchActor>();
		
		CStr FileName = CFile::fs_GetExpandedPath(Script, _pApplication->f_GetDirectory());
	
		auto fReportError = [pState, Description](CStr const &_Error)
			{
				if (pState->m_bReplied)
					return;
				pState->m_Continuation.f_SetException(DMibErrorInstance(_Error));
				pState->f_Replied();
			}
		;
		
		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "[{}] Launch update script"
				, Description
			)
		;
		
		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				"bash"
				, fg_CreateVector<CStr>(FileName, _Param)
				, _pApplication->f_GetDirectory()
				, [this, pState, Description, fReportError](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (!pState->m_LaunchActor)
						return;
					
					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							DMibLogWithCategory
								(
									Malterlib/Cloud/AppManager
									, Info
									, "[{}] Launched update script"
									, Description
								)
							;
						}
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
							fReportError(fg_Format("Failed to launch bash script: {}", LaunchError));
						}
						break;
					case NProcess::EProcessLaunchState_Exited:
						{
							auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
							if (ExitStatus != 0)
							{
								auto ErrorOutput = pState->m_AllOutput.f_Trim();
								if (ErrorOutput.f_IsEmpty())
									fReportError(fg_Format("Exit status: {}", ExitStatus));
								else
									fReportError(fg_Format("Exit status: {}{\n}{\n}{}", ExitStatus, ErrorOutput));
							}
							else
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/AppManager
										, Info
										, "[{}] Bash script exited with success"
										, Description
									)
								;
								if (!pState->m_bReplied)
								{
									pState->m_Continuation.f_SetResult(true);
									pState->f_Replied();
								}
							}
						}
						break;
					}
				}
			)
		;
		
		LaunchParams.m_fOnOutput = [this, pState, Description](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
			{
				if (!pState->m_LaunchActor)
					return;
				if (_Output.f_IsEmpty())
					return;
				DMibLogCategory(Malterlib/Cloud/AppManager);
				auto Output = _Output.f_TrimRight("\r\n");
				NMib::NLog::CSysLogCatScope AppScope(NMib::fg_GetSys()->f_GetLogger(), Description);
				if (_OutputType == EProcessLaunchOutputType_StdOut)
				{
					DMibLog(Info, "{}", Output);
					pState->m_StdOutput += _Output;
					pState->m_AllOutput += _Output;
				}
				else
				{
					DMibLog(Error, "{}", Output);
					pState->m_ErrorOutput += _Output;
					pState->m_AllOutput += _Output;
				}
			}
		;
		
		LaunchParams.m_RunAsUser = _pApplication->m_Settings.m_RunAsUser;
		LaunchParams.m_RunAsGroup = _pApplication->m_Settings.m_RunAsGroup;
		LaunchParams.m_Environment["HOME"] = _pApplication->f_GetDirectory() + "/.home";
		LaunchParams.m_Environment["TMPDIR"] = _pApplication->f_GetDirectory() + "/.tmp";
		
		LaunchParams.m_Environment["MalterlibCloud_TimeSinceStart"] = fg_Format("{fe1}", _TimeSinceUpdateStart);
		LaunchParams.m_Environment["MalterlibCloud_Application"] = _pApplication->m_Name;
		LaunchParams.m_Environment["MalterlibCloud_VersionApplication"] = _pApplication->m_Settings.m_VersionManagerApplication;
		if (_VersionID.f_IsValid())
		{
			LaunchParams.m_Environment["MalterlibCloud_Version"] = CStr::fs_ToStr(_VersionID);
			LaunchParams.m_Environment["MalterlibCloud_VersionID"] = CStr::fs_ToStr(_VersionID.m_VersionID);
			LaunchParams.m_Environment["MalterlibCloud_VersionBranch"] = _VersionID.m_VersionID.m_Branch;
			LaunchParams.m_Environment["MalterlibCloud_VersionMajor"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Major);
			LaunchParams.m_Environment["MalterlibCloud_VersionMinor"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Minor);
			LaunchParams.m_Environment["MalterlibCloud_VersionRevision"] = CStr::fs_ToStr(_VersionID.m_VersionID.m_Revision);
			LaunchParams.m_Environment["MalterlibCloud_VersionPlatform"] = _VersionID.m_Platform;
		}
		else
		{
			LaunchParams.m_Environment["MalterlibCloud_Version"] = "Unknown";
			LaunchParams.m_Environment["MalterlibCloud_VersionID"] = "Unknown";
		}
		
		if (_pVersionInformation)
		{
			LaunchParams.m_Environment["MalterlibCloud_Time"] = fg_Format("{}", _pVersionInformation->m_Time.f_ToLocal());
			LaunchParams.m_Environment["MalterlibCloud_Configuration"] = fg_Format("{}", _pVersionInformation->m_Configuration);
			LaunchParams.m_Environment["MalterlibCloud_Tags"] = fg_Format("{vs,vb}", _pVersionInformation->m_Tags);
			LaunchParams.m_Environment["MalterlibCloud_ExtraInfo"] = _pVersionInformation->m_ExtraInfo.f_ToString("");
			LaunchParams.m_Environment["MalterlibCloud_NumFiles"] = fg_Format("{}", _pVersionInformation->m_nFiles);
			LaunchParams.m_Environment["MalterlibCloud_NumBytes"] = fg_Format("{}", _pVersionInformation->m_nBytes);
		}
		
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bAllowExecutableLocate = true;
		
		pState->m_LaunchActor
			(
				&CProcessLaunchActor::f_Launch
				, LaunchParams
				, fg_ThisActor(this)
			)
			> [this, pState, Description, fReportError](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				if (!pState->m_LaunchActor)
					return;
				if (!_Subscription)
				{
					fReportError(fg_Format("[{}] Failed to launch bash script: {}", Description, _Subscription.f_GetExceptionStr()));
					return;
				}
				pState->m_LaunchSubscription = fg_Move(*_Subscription);
			}
		;
		
		return pState->m_Continuation;
	}

	NConcurrency::TCContinuation<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Update(NStr::CStr const &_Name, CApplicationUpdate const &_Update)
	{
		return m_pThis->fp_UpdateApplication
			(
				_Name
				, _Update
				, {}
				, [_Name](CStr const &_Info) 
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Update application '{}': {}", _Name, _Info);
				}
			)
		;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_UpdateApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		
		CStr Package;
		CAppManagerInterface::CApplicationUpdate Update;
		
		if (auto *pValue = _Params.f_GetMember("Package"))
		{
			Package = _Params["Package"].f_String();
			if (Package.f_IsEmpty())
				return DMibErrorInstance("You have to specify a package");
			Package = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory()));
		}
		else
		{
			Update.m_bDryRun = _Params["DryRun"].f_Boolean();
			Update.m_bUpdateSettings = _Params["UpdateSettings"].f_Boolean();
			
			CStr Version = _Params["Version"].f_String();
			if (auto *pValue = _Params.f_GetMember("VersionManagerPlatform"))
				Update.m_Platform = pValue->f_String(); 

			if (auto *pValue = _Params.f_GetMember("RequiredTags"))
			{
				TCSet<CStr> RequiredTags;
				for (auto &TagJSON : pValue->f_Array())
					RequiredTags[TagJSON.f_String()];
				Update.m_RequireTags = fg_Move(RequiredTags);
			}
			
			if (!Version.f_IsEmpty())
			{
				CStr ErrorStr;
				CVersionManager::CVersionID VersionID;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, ErrorStr, &VersionID))
					return DMibErrorInstance(fg_Format("Invalid version ID format: {}", ErrorStr));
				Update.m_Version = VersionID;
			}
		}
		
		fp_UpdateApplication
			(
				Name
				, Update
				, Package
				, [pResult, Name](CStr const &_Info)
				{
					pResult->f_AddStdOut(_Info + DMibNewLine);
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Update application '{}': {}", Name, _Info);
				}
			)
			> [pResult, Continuation, Name](TCAsyncResult<> &&_Result)
			{
				pResult->f_AddAsyncResult(_Result);
				Continuation.f_SetResult(fg_Move(*pResult));
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication
		(
			CStr const &_Name
			, CAppManagerInterface::CApplicationUpdate const &_Update
			, CStr const &_FromFileName
			, TCFunction<void (CStr const &_Info)> &&_fOnInfo
			, bool _bCheckPermissions
		)
	{
		// 1. Extract new application to temporary directory
		// 2. Stop old application
		// 3. Delete old files
		// 4. Move files from temporary directory to final destination
		// 5. Re-launch application

		auto Auditor = f_Auditor();

		if (_bCheckPermissions)
		{
			auto CallingHostID = fg_GetCallingHostID();
			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationUpdate"))
				return Auditor.f_AccessDenied("(Application update, command)");

			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)))
				return Auditor.f_AccessDenied("(Application update, app name)");
		}
		
		auto pOldApplication = mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->f_IsInProgress())
			return Auditor.f_Exception("Operation already in progress for application");

		auto InProgressScope = pApplication->f_SetInProgress();
		
		bool bDownloadVersion = true;
		bool bUpdateSettings = false;
		bool bDryRun = false;
		CStr Package;
		CStr Platform = pApplication->m_LastInstalledVersion.m_Platform;
		TCSet<CStr> RequiredTags;
		CVersionManager::CVersionIDAndPlatform VersionID;
		if (!_FromFileName.f_IsEmpty())
			bDownloadVersion = false;
		else
		{
			bDryRun = _Update.m_bDryRun;
			bUpdateSettings = _Update.m_bUpdateSettings; 
			if (_Update.m_Platform)
				Platform = *_Update.m_Platform;

			if (!CVersionManager::fs_IsValidPlatform(Platform))
				return Auditor.f_Exception("Invalid version platform format");
			
			auto RequiredTags = pApplication->m_Settings.m_AutoUpdateTags;
			auto AllowedBranches = pApplication->m_Settings.m_AutoUpdateBranches;
			
			if (_Update.m_RequireTags)
			{
				for (auto &Tag : *_Update.m_RequireTags)
				{
					if (!CVersionManager::fs_IsValidTag(Tag))
						return Auditor.f_Exception(fg_Format("'{}' is not a valid tag", Tag));
				}
				RequiredTags = *_Update.m_RequireTags;
			}
			
			if (AllowedBranches.f_IsEmpty())
				AllowedBranches = fg_CreateSet(pApplication->m_LastInstalledVersion.m_VersionID.m_Branch);
			
			if (_Update.m_Version)
			{
				CStr ErrorStr;
				VersionID.m_Platform = Platform;
				if (!_Update.m_Version->f_IsValid())
					return Auditor.f_Exception(fg_Format("Invalid version: {}", *_Update.m_Version));
				VersionID.m_VersionID = *_Update.m_Version; 
			}
			else
			{
				CStr Error;
				VersionID = fp_FindVersion(pApplication, RequiredTags, AllowedBranches, Platform, Error, EFindVersionFlag_RetryFailed);
				if (!VersionID.f_IsValid())
					return Auditor.f_Exception(Error);
			}
		}
		
		TCContinuation<void> Continuation;
		
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		TCSharedPointer<TCSharedPointer<CVersionManager::CVersionInformation>> pVersionInfo = fg_Construct();
		TCSharedPointer<NTime::CClock> pClock = fg_Construct(true); 
		
		auto fLogError = [this, _Name, pClock, pResult, Continuation, pApplication, VersionID, pVersionInfo, Auditor](CStr const &_Error, bool _bUnencrypted = true)
			{
				fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_Failed, VersionID, _Error);
		
				Continuation.f_SetException(Auditor.f_Exception(_Error));
				
				if (!pApplication->m_bDeleted && _bUnencrypted)
				{
					fp_RunUpdateScript(pApplication, EUpdateScript_OnError, _Error, VersionID, pVersionInfo->f_Get(), pClock->f_GetTime()) > [](TCAsyncResult<bool> &&_Result)
						{
							if (!_Result)
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/AppManager
										, Warning
										, "Error script failed: {}"
										, _Result.f_GetExceptionStr()
									)
								;
							}
							else if (*_Result)
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/AppManager
										, Info
										, "Successfully ran error script"
									)
								;
							}
						}
					;
				}
				else
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Info
							, "Skipped error script: bDeleted {}   bUnencrypted {}"
							, pApplication->m_bDeleted  
							, _bUnencrypted  
						)
					;
				}
			}
		;

		fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_ChangeEncryption, VersionID, {});
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Open, false) > [=](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr()), false);
					return;
				}
				
				CStr DownloadDirectory = fg_Format("{}/TempVersionDownload", pApplication->f_GetDirectory());
				
				auto DownloadDirectoryCleanup = fg_OnScopeExitActor
					(
						mp_FileActor
						,  [DownloadDirectory, bDownloadVersion]
						{
							if (!bDownloadVersion)
								return;
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
				
				auto fUnpackApp = [=]
					(
						CStr const &_SourcePath
						, TCSet<CStr> const &_AllowSourceExist
						, TCSharedPointer<CApplicationSettings> const &_pNewSettings
						, TCFunction <void ()> const &_fUpdateVersionInfo
					)
					{
						fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_Unpack, VersionID, {});
						
						CStr TemporaryDirectory = fg_Format("{}/TempVersion", pApplication->f_GetDirectory());
						
						auto TemporaryDirectoryCleanup = fg_OnScopeExitActor
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
						g_Dispatch(mp_FileActor) > [=, Settings = _pNewSettings ? *_pNewSettings : pApplication->m_Settings, OutputDirectory = pApplication->f_GetDirectory()]()
							{
								// 1. Extract new application to temporary directory
								fsp_CreateApplicationUserGroup(Settings, _fOnInfo, OutputDirectory);
								
								_fOnInfo("Extracting application to temporary directory");
								
								CStr SourcePath = _SourcePath;
								if (CFile::fs_FileExists(_SourcePath, EFileAttrib_Directory))
								{
									auto Files = CFile::fs_FindFiles(_SourcePath + "/*");
									if (Files.f_GetLen() == 1 && Files[0].f_Right(7) == ".tar.gz")
										SourcePath = Files[0];
								}
								
								// Cleanup any old crash version
								if (CFile::fs_FileExists(TemporaryDirectory))
									CFile::fs_DeleteDirectoryRecursive(TemporaryDirectory);
								
								TCVector<CStr> Files;
								CStr Output = fsp_UnpackApplication(SourcePath, TemporaryDirectory, pApplication->m_Name, Settings, Files, _AllowSourceExist, false);
								if (!Output.f_IsEmpty())
									_fOnInfo(Output);
								return Files;
							}
							> [=]
							(TCAsyncResult<TCVector<CStr>> &&_Files)
							{
								if (!_Files)
								{
									fLogError(fg_Format("Failed to unpack application: {}", _Files.f_GetExceptionStr()));
									return;
								}
								if (pApplication->m_bDeleted)
								{
									fLogError("Application has been deleted, aborting");
									return;
								}
								
								if (bDryRun)
								{
									_fOnInfo("Skipping stop, update and restart because of dry run");
									Auditor.f_Info("Update application (dry run)");
									Continuation.f_SetResult();
									return;
								}

								auto Files = fg_Move(*_Files);
								
								fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_StopOldApp, VersionID, {});
								_fOnInfo("Stopping old application");
								// 2. Stop old application
								pApplication->f_Stop(false) 
									> [=]
									(TCAsyncResult<uint32> &&_ExitStatus)
									{
										NStr::CStr Error = fp_GetApplicationStopErrors(_ExitStatus, pApplication->m_Name);
				
										if (!Error.f_IsEmpty())
											Auditor.f_Warning(Error);
										
										if (!_ExitStatus)
										{
											fLogError("Failed to exit old application, aborting update");
											return;
										}
										
										if (pApplication->m_bDeleted)
										{
											fLogError("Application has been deleted, aborting");
											return;
										}
										
										fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_PreUpdateScript, VersionID, {});
										
										fp_RunUpdateScript(pApplication, EUpdateScript_PreUpdate, CStr{}, VersionID, pVersionInfo->f_Get(), pClock->f_GetTime()) 
											> [=](TCAsyncResult<bool> &&_Result)
											{
												if (!_Result)
												{
													fLogError(fg_Format("Pre update script failed. {}", _Result.f_GetExceptionStr()));
													return;
												}
												
												fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_UpdateApplicationFiles, VersionID, {});
												_fOnInfo("Updating application files");
												g_Dispatch(mp_FileActor) >
													[
														=
														, OutputDirectory = pApplication->f_GetDirectory()
														, OldFiles = pApplication->m_Files
														, TemporaryDirectoryCleanup = TemporaryDirectoryCleanup 
														, DownloadDirectoryCleanup = DownloadDirectoryCleanup
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
													> [=](TCAsyncResult<void> &&_Result)
													{
														if (!_Result)
														{
															fLogError(fg_Format("Failed update application files. Please manually fix error and retry: {}", _Result.f_GetExceptionStr()));
															return;
														}
														if (pApplication->m_bDeleted)
														{
															fLogError("Application has been deleted, aborting");
															return;
														}
															
														fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_SaveApplicationState, VersionID, {});
														_fOnInfo("Saving application state");
														if (_pNewSettings)
															pApplication->m_Settings = *_pNewSettings;
														if (_fUpdateVersionInfo)
															_fUpdateVersionInfo();
														fp_UpdateApplicationJSON(pApplication) 
															> [=](TCAsyncResult<void> &&_Result)
															{
																if (!_Result)
																{
																	fLogError(fg_Format("Failed to save state: {}", _Result.f_GetExceptionStr()));
																	return;
																}
																
																fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_PostUpdateScript, VersionID, {});
																
																fp_RunUpdateScript
																	(
																		pApplication
																		, EUpdateScript_PostUpdate
																		, CStr{}
																		, VersionID
																		, pVersionInfo->f_Get()
																		, pClock->f_GetTime()
																	)
																	> [=](TCAsyncResult<bool> &&_Result)
																	{
																		if (!_Result)
																		{
																			fLogError
																				(
																					fg_Format
																					(
																						"Post update script failed. {}"
																						, _Result.f_GetExceptionStr()
																					)
																				)
																			;
																			return;
																		}
																		// 5. Re-launch application
																		_fOnInfo("Launching updated application");
																		pApplication->m_bJustUpdated = true;
																		fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_StartNewApp, VersionID, {});
																		fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
																			> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
																			{
																				if (!_Result)
																				{
																					fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
																					return;
																				}
																				if (VersionID.f_IsValid())
																					_fOnInfo(fg_Format("Application was successfully updated to version {}", VersionID));
																				else
																					_fOnInfo("Application was successfully updated");
																				
																				fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_Finished, VersionID, {});
																				Auditor.f_Info("Update application");
																				Continuation.f_SetResult();
																				fp_RunUpdateScript
																					(
																						pApplication
																						, EUpdateScript_PostLaunch
																						, CStr{}
																						, VersionID
																						, pVersionInfo->f_Get()
																						, pClock->f_GetTime()
																					) 
																					> [=](TCAsyncResult<bool> &&_Result)
																					{
																						fp_OnUpdateEvent
																							(
																								pApplication->m_Name
																								, CAppManagerInterface::EUpdateStage_PostLaunchScriptFinished
																								, VersionID
																								, {}
																							)
																						;
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
																					}
																				;
																			}
																		;
																	}
																;
															}
														;
													}
												;
											}
										;
									}
								;
							}
						;
					}
				;
				
				if (bDownloadVersion)
				{
					_fOnInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
					fp_OnUpdateEvent(pApplication->m_Name, CAppManagerInterface::EUpdateStage_DownloadVersion, VersionID, {});
		
					self(&CAppManagerActor::fp_DownloadApplication, pApplication->m_Settings.m_VersionManagerApplication, VersionID, DownloadDirectory) 
						> [=, DownloadDirectoryCleanup=DownloadDirectoryCleanup](TCAsyncResult<CVersionManager::CVersionInformation> &&_VersionInfo)
						{
							if (!_VersionInfo)
							{
								fLogError(fg_Format("Failed to download application from version manager: {}", _VersionInfo.f_GetExceptionStr()));
								return;
							}
							
							if (pApplication->m_bDeleted)
							{
								fLogError("Application has been deleted, aborting");
								return;
							}

							_fOnInfo(fg_Format("Application downloaded, unpacking"));
							
							auto &VersionInfo = *_VersionInfo;

							*pVersionInfo = fg_Construct(VersionInfo);
							
							for (auto &Tag : RequiredTags)
							{
								if (!VersionInfo.m_Tags.f_FindEqual(Tag))
								{
									fLogError(fg_Format("Missing required tag: '{}'", Tag));
									return;
								}
							}
							
							TCSharedPointer<CApplicationSettings> pNewSettings;
							
							TCFunction <void ()> fUpdateVersionInfo;
							
							if (!bDryRun)
							{
								if (bUpdateSettings)
								{
									CApplicationSettings NewSettings = pApplication->m_Settings;
									EApplicationSetting NewChangedSettings = EApplicationSetting_None;
									NewSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
									
									CStr Error;
									if (!NewSettings.f_Validate(Error))
									{
										fLogError(fg_Format("Updating settings resulted in invalid settings: {}", Error));
										Continuation.f_SetException(Auditor.f_Exception(Error));
										return;
									}
									pNewSettings = fg_Construct(NewSettings);
								}
								
								pApplication->m_LastTriedInstalledVersion = VersionID;
								pApplication->m_LastTriedInstalledVersionInfo = VersionInfo;
								
								fUpdateVersionInfo = [pApplication, VersionID, VersionInfo]
									{
										pApplication->m_LastInstalledVersion = VersionID;
										pApplication->m_LastInstalledVersionInfo = VersionInfo;
									}
								;
							}
							
							fUnpackApp(DownloadDirectory, fg_CreateSet(DownloadDirectory), pNewSettings, fUpdateVersionInfo);
						}
					;
				}
				else
					fUnpackApp(_FromFileName, {}, {}, {});
			}
		;
		
		return Continuation;
	}
}
