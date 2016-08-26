// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib
{
	namespace NCloud
	{
		namespace NAppManager
		{
			void CAppManagerActor::fp_OutputApplicationStop(TCAsyncResult<uint32> const &_Result, CDistributedAppCommandLineResults &o_Results, CStr const &_Name)
			{
				if (!_Result)
					o_Results.f_AddStdErr(fg_Format("Error stopping application '{}'{\n}", _Result.f_GetExceptionStr()));
				else if (*_Result)
					o_Results.f_AddStdErr(fg_Format("Application '{}' exited with non 0 status: {}{\n}", _Name, *_Result));
			}
			
			constexpr static EFileAttrib gc_RootAttributes = 
						EFileAttrib_UnixAttributesValid  
						| EFileAttrib_UserRead 
						| EFileAttrib_UserWrite
						| EFileAttrib_UserExecute 
						| EFileAttrib_GroupRead
						| EFileAttrib_GroupExecute
						| EFileAttrib_EveryoneRead 
						| EFileAttrib_EveryoneExecute
			;
			
			void CAppManagerActor::fsp_UpdateAttributes(CStr const &_File)
			{
				auto CurrentAttributes = CFile::fs_GetAttributes(_File);
				CFile::fs_SetAttributes
					(
						_File
						, EFileAttrib_UnixAttributesValid 
						| (CurrentAttributes & EFileAttrib_UserRead) 
						| (CurrentAttributes & EFileAttrib_UserWrite)
						| (CurrentAttributes & EFileAttrib_UserExecute) 
						| (CurrentAttributes & EFileAttrib_GroupRead)
						| (CurrentAttributes & EFileAttrib_GroupExecute)
						| (CurrentAttributes & EFileAttrib_EveryoneRead)
						| (CurrentAttributes & EFileAttrib_EveryoneExecute)
					)
				;
			}

			CStr CAppManagerActor::fsp_UnpackApplication(CStr const &_Source, CStr const &_Destination, TCSharedPointer<CApplication> const &_pApplication, TCVector<CStr> &o_Files)
			{
				CStr Return;
				if (CFile::fs_FileExists(_Destination) && !CFile::fs_FindFiles(_Destination + "/*").f_IsEmpty())
					DMibError(fg_Format("Application already exists at: '{}'. You have to manually delete it to resue the name", _Destination));
				
				if (CFile::fs_FileExists(_Source, EFileAttrib_Directory))
					CFile::fs_DiffCopyFileOrDirectory(_Source, _Destination, nullptr);
				else
				{
					CFile::fs_CreateDirectory(_Destination);
					CStr Output = fsp_RunTool
						(
							CStr::CFormat("[{}] Extracting application") << _pApplication->m_Name
							, "tar"
							, _Destination
							, fg_CreateVector<CStr>("--no-same-owner", "-xf", _Source)
							, ""
							, ""
						)
					;
					Return = Output;
				}

				if (!_pApplication->m_RunAsUser.f_IsEmpty())
					CFile::fs_SetOwnerRecursive(_Destination, _pApplication->m_RunAsUser, false);
				if (!_pApplication->m_RunAsGroup.f_IsEmpty())
					CFile::fs_SetGroupRecursive(_Destination, _pApplication->m_RunAsGroup, false);
				
				CStr ExcutableFile = fg_Format("{}/{}", _Destination, _pApplication->m_Executable); 
				if (!CFile::fs_FileExists(ExcutableFile, EFileAttrib_Executable))
					DMibError(fg_Format("Executable file '{}' does not exist or does not have the executable flag set", ExcutableFile));
				
				CFile::CFindFilesOptions FindOptions(_Destination + "/*", true);
				FindOptions.m_AttribMask = EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link | EFileAttrib_FindDirectoryLast;
				
				auto Files = CFile::fs_FindFiles(FindOptions);
				
				CFile::fs_SetAttributes(_Destination, gc_RootAttributes);
				
				mint PrefixLen = _Destination.f_GetLen() + 1;
				for (auto &File : Files)
				{
					fsp_UpdateAttributes(File.m_Path);
					o_Files.f_Insert(File.m_Path.f_Extract(PrefixLen));
				}
				
				return Return;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_AddApplication(CEJSON const &_Params)
			{
				TCSharedPointer<CApplication> pApplication = fg_Construct(_Params["Name"].f_String(), this);
				bool bForceOverwrite = _Params["ForceOverwrite"].f_Boolean();
				
				pApplication->m_EncryptionStorage = _Params["EncryptionStorage"].f_String();
				pApplication->m_Executable = _Params["Executable"].f_String();
				for (auto &Parameter : _Params["ExecutableParameters"].f_Array())
					pApplication->m_ExecutableParameters.f_Insert(Parameter.f_String());
				pApplication->m_RunAsUser = _Params["RunAsUser"].f_String(); 
				pApplication->m_RunAsGroup = _Params["RunAsGroup"].f_String();
				
				auto Directory = pApplication->f_GetDirectory();
				CStr Package = _Params["Package"].f_String();
				
				if (Package.f_IsEmpty())
					return DMibErrorInstance("You have to specify a package");
				
				Package = CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory());
				
				if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
				{
					if (pApplicationsState->f_GetMember(pApplication->m_Name))
						return DMibErrorInstance(fg_Format("Application with name '{}' already exists", pApplication->m_Name));
				}
				
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
				
				auto fLogInfo = [pResult](CStr const &_Info)
					{
						pResult->f_AddStdOut(_Info + DMibNewLine);
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
					}
				;
				auto fLogError = [pResult, Continuation](CStr const &_Error)
					{
						pResult->f_AddStdErr(_Error + DMibNewLine);
						pResult->m_Status = 1;
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				
				fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Setup, bForceOverwrite) 
					> [this, fLogError, fLogInfo, pApplication, Directory, Package, pResult, Continuation](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							fLogError(_Result.f_GetExceptionStr());
							return;
						}
						fg_Dispatch
							(
								mp_FileActor
								, [pApplication, Directory, Package, pResult, fLogInfo]()
								{
									if (!pApplication->m_RunAsGroup.f_IsEmpty())
									{
										CStr GroupID;
										if (!NSys::fg_UserManagement_GroupExists(pApplication->m_RunAsGroup, GroupID))
										{
											NSys::fg_UserManagement_CreateGroup(pApplication->m_RunAsGroup, GroupID);
											fLogInfo(fg_Format("Created group '{}' with resulting group ID: {}", pApplication->m_RunAsGroup, GroupID));
										}
									}
									if (!pApplication->m_RunAsUser.f_IsEmpty())
									{
										CStr UserID;
										if (!NSys::fg_UserManagement_UserExists(pApplication->m_RunAsUser, UserID))
										{
											NSys::fg_UserManagement_CreateUser
												(
													pApplication->m_RunAsGroup
													, pApplication->m_RunAsUser
													, ""
													, pApplication->m_RunAsUser
													, Directory
													, UserID
												)
											;
											fLogInfo(fg_Format("Created user '{}' with resulting user ID: {}", pApplication->m_RunAsUser, UserID));
										}
									}

									TCVector<CStr> Files;
									CStr Output = fsp_UnpackApplication(Package, Directory, pApplication, Files);
									if (!Output.f_IsEmpty())
										pResult->f_AddStdOut(Output);
									
									return Files;
								}
							)
							> [this, pResult, pApplication, Continuation, fLogInfo, fLogError](TCAsyncResult<TCVector<CStr>> &&_Files)
							{
								if (!_Files)
								{
									fLogError(fg_Format("Failed to unpack application: {}", _Files.f_GetExceptionStr()));
									return;
								}
								if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
								{
									if (pApplicationsState->f_GetMember(pApplication->m_Name))
									{
										fLogError(fg_Format("Application with name '{}' already exists", pApplication->m_Name));
										return;
									}
								}
								
								pApplication->m_Files = fg_Move(*_Files);
								auto &ApplicationJSON = mp_State.m_StateDatabase.m_Data["Applications"][pApplication->m_Name];
								ApplicationJSON["Executable"] = pApplication->m_Executable; 
								ApplicationJSON["RunAsUser"] = pApplication->m_RunAsUser; 
								ApplicationJSON["RunAsGroup"] = pApplication->m_RunAsGroup;
								auto &Parameters = ApplicationJSON["Parameters"].f_Array();
								for (auto &Parameter : pApplication->m_ExecutableParameters)
									Parameters.f_Insert(Parameter);
								ApplicationJSON["EncryptionStorage"] = pApplication->m_EncryptionStorage;
								ApplicationJSON["Files"] = *_Files;
								
								mp_Applications[pApplication->m_Name] = pApplication;
								auto InProgressScope = pApplication->f_SetInProgress();

								mp_State.m_StateDatabase.f_Save() 
									> [this, Continuation, pResult, fLogError, fLogInfo, InProgressScope, pApplication](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
										{
											fLogError(fg_Format("Failed to save state: {}", _Result.f_GetExceptionStr()));
											return;
										}
										fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false) 
											> [fLogError, fLogInfo, Continuation, InProgressScope, pResult](TCAsyncResult<void> &&_Result)
											{
												if (!_Result)
												{
													fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
													return;
												}
												fLogInfo("Application was successfully added");
												Continuation.f_SetResult(fg_Move(*pResult));
											}
										;
									}
								;
							}
						;
					}
				;
				
				return Continuation;
			}

			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RemoveApplication(CEJSON const &_Params)
			{
				CStr Name = _Params["Name"].f_String();
				auto *pApplication = mp_Applications.f_FindEqual(Name);
				if (!pApplication)
					return DMibErrorInstance(fg_Format("No such application '{}'", Name));
				
				auto &Application = **pApplication;
				if (Application.m_bOperationInProgress)
					return DMibErrorInstance("Operation already in progress for application");
				auto InProgressScope = Application.f_SetInProgress();
					
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				Application.f_Stop(true) > [this, Continuation, Name, InProgressScope](TCAsyncResult<uint32> &&_Result)
					{
						CDistributedAppCommandLineResults Results;
						fp_OutputApplicationStop(_Result, Results, Name);
						
						auto *pApplication = mp_Applications.f_FindEqual(Name);
						if (!pApplication)
							return Continuation.f_SetException(DMibErrorInstance(fg_Format("No such application '{}'", Name)));
						
						(*pApplication)->f_Delete();
						mp_Applications.f_Remove(Name);

						if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
						{
							if (pApplicationsState->f_GetMember(Name))
								pApplicationsState->f_RemoveMember(Name);
						}
						
						mp_State.m_StateDatabase.f_Save() > [Results = fg_Move(Results), Continuation, InProgressScope](TCAsyncResult<void> &&_Result) mutable
							{
								if (!_Result)
								{
									Results.f_AddStdErr(fg_Format("Failed to save state: {}{\n}", _Result.f_GetExceptionStr()));
									Results.m_Status = 1;
									Continuation.f_SetResult(fg_Move(Results));
									return;
								}
								
								Continuation.f_SetResult(fg_Move(Results));
							}
						;
					}
				;
				
				return Continuation;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_UpdateApplication(CEJSON const &_Params)
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
				
				CStr Package = _Params["Package"].f_String();
				
				if (Package.f_IsEmpty())
					return DMibErrorInstance("You have to specify a package");
				
				Package = CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory());
				
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				
				TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
				
				auto fLogInfo = [pResult](CStr const &_Info)
					{
						pResult->f_AddStdOut(_Info + DMibNewLine);
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
					}
				;
				auto fLogError = [pResult, Continuation](CStr const &_Error)
					{
						pResult->f_AddStdErr(_Error + DMibNewLine);
						pResult->m_Status = 1;
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;

				fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Open, false)
					> [this, pApplication, Package, fLogInfo, fLogError, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							fLogError(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr()));
							return;
						}
						
						CStr TemporaryDirectory = fg_Format("{}/{}", pApplication->f_GetDirectory(), fg_RandomID());
						
						auto TemporaryDirectoryCleanup = fg_OnScopeExitShared
							(
								[FileActor = mp_FileActor, TemporaryDirectory]
								{
									fg_Dispatch
										(
											FileActor
											, [TemporaryDirectory]
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
										> fg_DiscardResult()
									;
								}
							)
						;
						
						fg_Dispatch
							(
								mp_FileActor
								, [pApplication, TemporaryDirectory, Package, fLogInfo, InProgressScope, TemporaryDirectoryCleanup]()
								{
									// 1. Extract new application to temporary directory
									fLogInfo("Extracting application to temporary directory");
									TCVector<CStr> Files;
									CStr Output = fsp_UnpackApplication(Package, TemporaryDirectory, pApplication, Files);
									if (!Output.f_IsEmpty())
										fLogInfo(Output);
									return Files;
								}
							)
							> [this, pResult, pApplication, Continuation, fLogInfo, fLogError, InProgressScope, TemporaryDirectory, TemporaryDirectoryCleanup](TCAsyncResult<TCVector<CStr>> &&_Files)
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

								auto Files = fg_Move(*_Files);
								
								fLogInfo("Stopping old application");
								// 2. Stop old application
								pApplication->f_Stop(false) > [this, pApplication, pResult, fLogInfo, fLogError, Continuation, TemporaryDirectory, InProgressScope, Files, TemporaryDirectoryCleanup]
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
										
										fLogInfo("Updating application files");
										fg_Dispatch
											(
												mp_FileActor
												,
												[
													pApplication
													, TemporaryDirectory
													, OutputDirectory = pApplication->f_GetDirectory()
													, pResult
													, Files
													, OldFiles = pApplication->m_Files
													, TemporaryDirectoryCleanup
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
													auto fSetOwners = [&](CStr _Directory)
														{
															while (_Directory.f_GetLen() >= OutputDirectory.f_GetLen())
															{
																if (!pApplication->m_RunAsGroup.f_IsEmpty())
																	CFile::fs_SetOwner(_Directory, pApplication->m_RunAsUser);
																if (!pApplication->m_RunAsGroup.f_IsEmpty())
																	CFile::fs_SetGroup(_Directory, pApplication->m_RunAsGroup);
																_Directory = CFile::fs_GetPath(_Directory);
															}
														}
													;
													for (auto &File : Files)
													{
														CStr SourcePath = fg_Format("{}/{}", TemporaryDirectory, File);
														CStr DestinationPath = fg_Format("{}/{}", OutputDirectory, File);
														if (CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
														{
															CStr Directory = CFile::fs_GetPath(DestinationPath);
															CFile::fs_CreateDirectory(Directory);
															fSetOwners(Directory);
															CFile::fs_RenameFile(SourcePath, DestinationPath);
														}
													}
													CFile::fs_CreateDirectory(OutputDirectory + "/.home");
													CFile::fs_CreateDirectory(OutputDirectory + "/.tmp");
													
													CFile::fs_SetAttributes(OutputDirectory, gc_RootAttributes);
												}
											)
											> [this, pApplication, InProgressScope, fLogError, fLogInfo, Files, pResult, Continuation](TCAsyncResult<void> &&_Result)
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
													
												// 5. Re-launch application
												auto &ApplicationJSON = mp_State.m_StateDatabase.m_Data["Applications"][pApplication->m_Name];
								
												pApplication->m_Files = Files;
												ApplicationJSON["Files"] = Files;
												
												fLogInfo("Saving application state");
												mp_State.m_StateDatabase.f_Save() 
													> [this, Continuation, pResult, pApplication, fLogInfo, fLogError, InProgressScope](TCAsyncResult<void> &&_Result)
													{
														if (!_Result)
														{
															fLogError(fg_Format("Failed to save state: {}", _Result.f_GetExceptionStr()));
															return;
														}
														
														fLogInfo("Launching updated application");
														fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
															> [fLogError, fLogInfo, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
															{
																if (!_Result)
																{
																	fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
																	return;
																}
																fLogInfo("Application was successfully updated");
																Continuation.f_SetResult(fg_Move(*pResult));
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
				
				return Continuation;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_StopApplication(CEJSON const &_Params)
			{
				CStr Name = _Params["Name"].f_String();
				auto pOldApplication = mp_Applications.f_FindEqual(Name);
				if (!pOldApplication)
					return DMibErrorInstance(fg_Format("No such application '{}'", Name));
				
				TCSharedPointer<CApplication> pApplication = *pOldApplication;
				
				if (pApplication->m_bOperationInProgress)
					return DMibErrorInstance("Operation already in progress for application");
				if (!pApplication->m_ProcessLaunch || pApplication->m_bStopped)
					return DMibErrorInstance("Application already stopped");

				auto InProgressScope = pApplication->f_SetInProgress();
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
				auto fLogError = [pResult, Continuation](CStr const &_Error)
					{
						pResult->f_AddStdErr(_Error + DMibNewLine);
						pResult->m_Status = 1;
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				
				pApplication->f_Stop(false) > [this, pApplication, pResult, fLogError, Continuation, InProgressScope]
					(TCAsyncResult<uint32> &&_ExitStatus)
					{
						fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
						
						if (!_ExitStatus)
						{
							fLogError("Failed to exit old application");
							return;
						}
						
						if (pApplication->m_bDeleted)
						{
							fLogError("Application has been deleted, aborting");
							return;
						}
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				return Continuation;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_StartApplication(CEJSON const &_Params)
			{
				CStr Name = _Params["Name"].f_String();
				auto pOldApplication = mp_Applications.f_FindEqual(Name);
				if (!pOldApplication)
					return DMibErrorInstance(fg_Format("No such application '{}'", Name));
				
				TCSharedPointer<CApplication> pApplication = *pOldApplication;
				
				if (pApplication->m_bOperationInProgress)
					return DMibErrorInstance("Operation already in progress for application");
				if (pApplication->m_ProcessLaunch)
					return DMibErrorInstance("Application already started");

				auto InProgressScope = pApplication->f_SetInProgress();
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
				auto fLogError = [pResult, Continuation](CStr const &_Error)
					{
						pResult->f_AddStdErr(_Error + DMibNewLine);
						pResult->m_Status = 1;
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				
				fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true)
					> [fLogError, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
							return;
						}
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				return Continuation;
			}
			
			TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RestartApplication(CEJSON const &_Params)
			{
				CStr Name = _Params["Name"].f_String();
				auto pOldApplication = mp_Applications.f_FindEqual(Name);
				if (!pOldApplication)
					return DMibErrorInstance(fg_Format("No such application '{}'", Name));
				
				TCSharedPointer<CApplication> pApplication = *pOldApplication;
				
				if (pApplication->m_bOperationInProgress)
					return DMibErrorInstance("Operation already in progress for application");

				auto InProgressScope = pApplication->f_SetInProgress();
				TCContinuation<CDistributedAppCommandLineResults> Continuation;
				TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
				auto fLogError = [pResult, Continuation](CStr const &_Error)
					{
						pResult->f_AddStdErr(_Error + DMibNewLine);
						pResult->m_Status = 1;
						Continuation.f_SetResult(fg_Move(*pResult));
					}
				;
				
				pApplication->f_Stop(false) > [this, pApplication, pResult, fLogError, Continuation, InProgressScope]
					(TCAsyncResult<uint32> &&_ExitStatus)
					{
						fp_OutputApplicationStop(_ExitStatus, *pResult, pApplication->m_Name);
						
						if (!_ExitStatus)
						{
							fLogError("Failed to exit old application");
							return;
						}
						
						if (pApplication->m_bDeleted)
						{
							fLogError("Application has been deleted, aborting");
							return;
						}

						fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, true)
							> [fLogError, Continuation, pResult, InProgressScope](TCAsyncResult<void> &&_Result)
							{
								if (!_Result)
								{
									fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
									return;
								}
								Continuation.f_SetResult(fg_Move(*pResult));
							}
						;
					}
				;
				return Continuation;
			}
		}
	}
}
