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
		}
		DMibNeverGetHere;
		return "Unknown";
	}

	TCContinuation<void> CAppManagerActor::fp_RunUpdateScript(TCSharedPointer<CApplication> const &_pApplication, EUpdateScript _Script)
	{
		CStr Script = _pApplication->m_UpdateScripts.f_GetScript(_Script);
		if (Script.f_IsEmpty())
			return fg_Explicit();
		
		struct CState
		{
			TCContinuation<void> m_Continuation;
			TCActor<CProcessLaunchActor> m_LaunchActor;
			CActorSubscription m_LaunchSubscription;
			CStr m_ErrorOutput;
			CStr m_StdOutput;
			
			bool m_bReplied = false;
			void f_Replied()
			{
				m_bReplied = true;
				m_LaunchActor->f_Destroy();
			}
		};
		
		CStr Description = fg_Format("{}/{}", _pApplication->m_Name, _pApplication->m_UpdateScripts.f_GetName(_Script));
		
		TCSharedPointer<CState> pState = fg_Construct();
		pState->m_LaunchActor = fg_ConstructActor<CProcessLaunchActor>();
		
		CStr FileName = fg_Format("{}/{}", _pApplication->f_GetDirectory(), Script);
	
		auto fReportError = [pState, Description](CStr const &_Error)
			{
				if (pState->m_bReplied)
					return;
				pState->m_Continuation.f_SetException(DMibErrorInstance(_Error));
				pState->f_Replied();
			}
		;
		
		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				"bash"
				, fg_CreateVector<CStr>(FileName)
				, CFile::fs_GetProgramDirectory()
				, [this, pState, Description, fReportError](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (!pState->m_LaunchActor)
						return;
					
					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Launched bash script '{}'", Description);
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
								auto ErrorOutput = pState->m_ErrorOutput.f_Trim();
								if (ErrorOutput.f_IsEmpty())
									fReportError(fg_Format("Bash script exited with error status: {}", ExitStatus));
								else
									fReportError(fg_Format("Bash script exited with error status: {}. Error output:{\n}{}", ExitStatus, ErrorOutput));
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
									pState->m_Continuation.f_SetResult();
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
				}
				else
				{
					DMibLog(Error, "{}", Output);
					pState->m_ErrorOutput += _Output;
				}
			}
		;
		
		LaunchParams.m_RunAsUser = _pApplication->m_RunAsUser;
		LaunchParams.m_RunAsGroup = _pApplication->m_RunAsGroup;
		LaunchParams.m_Environment["HOME"] = _pApplication->f_GetDirectory() + "/.home";
		LaunchParams.m_Environment["TMPDIR"] = _pApplication->f_GetDirectory() + "/.tmp";
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_bAllowExecutableLocate = true;
		
		pState->m_LaunchActor
			(
				&CProcessLaunchActor::f_Launch
				, LaunchParams
				, EProcessLaunchCloseFlag_StopProcess | EProcessLaunchCloseFlag_BlockOnExit
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
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_UpdateApplication(CEJSON const &_Params, bool _bFromAutoUpdate)
	{
		// 1. Extract new application to temporary directory
		// 2. Stop old application
		// 3. Delete old files
		// 4. Move files from temporary directory to final destination
		// 5. Re-launch application
		
		CStr Name = _Params["Name"].f_String();
		auto pOldApplication = mp_Applications.f_FindEqual(Name);
		if (!pOldApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->m_bOperationInProgress)
			return DMibErrorInstance("Operation already in progress for application");
		auto InProgressScope = pApplication->f_SetInProgress();
		
		bool bDownloadVersion = true;
		bool bUpdateSettings = false;
		bool bDryRun = false;
		CStr Package;
		TCSet<CStr> RequiredTags;
		CVersionManager::CVersionIdentifier VersionID;
		if (auto *pValue = _Params.f_GetMember("Package"))
		{
			CStr Package = _Params["Package"].f_String();
			if (Package.f_IsEmpty())
				return DMibErrorInstance("You have to specify a package");
			bDownloadVersion = false;
			Package = CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory());
		}
		else
		{
			bDryRun = _Params["DryRun"].f_Boolean();
			bUpdateSettings = _Params["UpdateSettings"].f_Boolean(); 
			CStr Version = _Params["Version"].f_String();

			auto RequiredTags = pApplication->m_AutoUpdateTags;
			auto AllowedBranches = pApplication->m_AutoUpdateBranches;			
			
			if (auto *pValue = _Params.f_GetMember("RequiredTags"))
			{
				RequiredTags.f_Clear();
				for (auto &TagJSON : pValue->f_Array())
				{
					auto &Tag = TagJSON.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));
					RequiredTags[Tag];
				}
			}
			
			if (AllowedBranches.f_IsEmpty())
				AllowedBranches = fg_CreateSet(pApplication->m_LastInstalledVersion.m_Branch);
			
			if (!Version.f_IsEmpty())
			{
				CStr ErrorStr;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, ErrorStr, &VersionID))
					return DMibErrorInstance(fg_Format("Invalid version ID format: {}", ErrorStr));
			}
			else
			{
				CStr Error;
				VersionID = fp_FindVersionForAutoUpdate(pApplication, RequiredTags, AllowedBranches, Error);
				if (!VersionID.f_IsValid())
					return DMibErrorInstance(Error);
			}
		}
		
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		
		auto fLogInfo = [pResult](CStr const &_Info)
			{
				pResult->f_AddStdOut(_Info + DMibNewLine);
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Update application: {}", _Info);
			}
		;
		auto fLogError = [pResult, Continuation, _bFromAutoUpdate](CStr const &_Error)
			{
				if (_bFromAutoUpdate)
				{
					Continuation.f_SetException(DMibErrorInstance(_Error));
				}
				else
				{
					pResult->f_AddStdErr(_Error + DMibNewLine);
					pResult->m_Status = 1;
					Continuation.f_SetResult(fg_Move(*pResult));
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (update application): {}", _Error);
				}
			}
		;

		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Open, false) > [=](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr()));
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
				
				auto fUnpackApp = [=](CStr const &_SourcePath, TCSet<CStr> const &_AllowSourceExist)
					{
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
						fg_Dispatch
							(
								mp_FileActor
								, [=]()
								{
									// 1. Extract new application to temporary directory
									fLogInfo("Extracting application to temporary directory");
									TCVector<CStr> Files;
									CStr Output = fsp_UnpackApplication(_SourcePath, TemporaryDirectory, pApplication, Files, _AllowSourceExist);
									if (!Output.f_IsEmpty())
										fLogInfo(Output);
									return Files;
								}
							)
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
									fLogInfo("Skipping stop, update and restart because of dry run");
									Continuation.f_SetResult(fg_Move(*pResult));
									return;
								}

								auto Files = fg_Move(*_Files);
								
								fLogInfo("Stopping old application");
								// 2. Stop old application
								pApplication->f_Stop(false) 
									> [=]
									(TCAsyncResult<uint32> &&_ExitStatus)
									{
										fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
										
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
										
										self(&CAppManagerActor::fp_RunUpdateScript, pApplication, EUpdateScript_PreUpdate) > [=](TCAsyncResult<void> &&_Result)
											{
												if (!_Result)
												{
													fLogError(fg_Format("Failed to run pre update script. Please manually fix error and retry: {}", _Result.f_GetExceptionStr()));
													return;
												}
												fLogInfo("Updating application files");
												fg_Dispatch
													(
														mp_FileActor
														,
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
													)
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
															
														fLogInfo("Saving application state");
														self(&CAppManagerActor::fp_UpdateApplicationJSON, pApplication) 
															> [=](TCAsyncResult<void> &&_Result)
															{
																if (!_Result)
																{
																	fLogError(fg_Format("Failed to save state: {}", _Result.f_GetExceptionStr()));
																	return;
																}
																
																self(&CAppManagerActor::fp_RunUpdateScript, pApplication, EUpdateScript_PostUpdate) > [=](TCAsyncResult<void> &&_Result)
																	{
																		if (!_Result)
																		{
																			fLogError
																				(
																					fg_Format
																					(
																						"Failed to run post update script. Please manually fix error and retry: {}"
																						, _Result.f_GetExceptionStr()
																					)
																				)
																			;
																			return;
																		}
																		// 5. Re-launch application
																		fLogInfo("Launching updated application");
																		fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
																			> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
																			{
																				if (!_Result)
																				{
																					fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
																					return;
																				}
																				if (VersionID.f_IsValid())
																					fLogInfo(fg_Format("Application was successfully updated to version {}", VersionID));
																				else
																					fLogInfo("Application was successfully updated");
																				Continuation.f_SetResult(fg_Move(*pResult));
																				self(&CAppManagerActor::fp_RunUpdateScript, pApplication, EUpdateScript_PostLaunch) 
																					> [=](TCAsyncResult<void> &&_Result)
																					{
																						if (!_Result)
																						{
																							DMibLogWithCategory
																								(
																									Malterlib/Cloud/AppManager
																									, Warning
																									, "Post update script failed: {}"
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
					fLogInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
					self(&CAppManagerActor::fp_DownloadApplication, pApplication->m_VersionManagerApplication, VersionID, DownloadDirectory) 
						> [=](TCAsyncResult<CVersionManager::CVersionInformation> &&_VersionInfo)
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

							fLogInfo(fg_Format("Application downloaded, unpacking"));
							
							auto &VersionInfo = *_VersionInfo;
							
							for (auto &Tag : RequiredTags)
							{
								if (!VersionInfo.m_Tags.f_FindEqual(Tag))
								{
									fLogError(fg_Format("Missing required tag: '{}'", Tag));
									return;
								}
							}
							
							if (!bDryRun)
							{
								if (bUpdateSettings)
								{
									auto &ExtraInfo = VersionInfo.m_ExtraInfo;
									if (auto *pValue = ExtraInfo.f_GetMember("Executable", EJSONType_String))
									{
										if (pValue->f_String().f_IsEmpty())
										{
											fLogError("Version info specifies Executable, but it's empty");
											return;
										}
										pApplication->m_Executable = pValue->f_String();
									}

									if (auto *pValue = ExtraInfo.f_GetMember("RunAsUser", EJSONType_String))
										pApplication->m_RunAsUser = pValue->f_String();

									if (auto *pValue = ExtraInfo.f_GetMember("RunAsGroup", EJSONType_String))
										pApplication->m_RunAsGroup = pValue->f_String();

									if (auto *pValue = ExtraInfo.f_GetMember("ExecutableParams", EJSONType_Array))
									{
										pApplication->m_ExecutableParameters.f_Clear();
										for (auto &Param : pValue->f_Array())
										{
											if (!Param.f_IsString())
												continue;
											
											pApplication->m_ExecutableParameters.f_Insert(Param.f_String());
										}
									}
								}
								pApplication->m_LastInstalledVersion = VersionID;
								pApplication->m_LastInstalledVersionInfo = VersionInfo;
							}
							
							fUnpackApp(DownloadDirectory, fg_CreateSet(DownloadDirectory));
						}
					;
				}
				else
					fUnpackApp(Package, {});
			}
		;
		
		return Continuation;
	}
}
