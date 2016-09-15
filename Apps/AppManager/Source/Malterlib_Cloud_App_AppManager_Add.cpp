// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_AddApplication(CEJSON const &_Params)
	{
		TCSharedPointer<CApplication> pApplication = fg_Construct(_Params["Name"].f_String(), this);
		bool bForceOverwrite = _Params["ForceOverwrite"].f_Boolean();
		
		pApplication->m_EncryptionStorage = _Params["EncryptionStorage"].f_String();
		if (auto *pValue = _Params.f_GetMember("Executable"))
			pApplication->m_Executable = pValue->f_String();
		if (auto *pValue = _Params.f_GetMember("ExecutableParameters"))
		{
			for (auto &Parameter : pValue->f_Array())
				pApplication->m_ExecutableParameters.f_Insert(Parameter.f_String());
		}
		if (auto *pValue = _Params.f_GetMember("RunAsUser"))
			pApplication->m_RunAsUser = pValue->f_String();
		if (auto *pValue = _Params.f_GetMember("RunAsGroup"))
			pApplication->m_RunAsGroup = pValue->f_String();
		CStr Version;
		if (auto *pValue = _Params.f_GetMember("Version"))
			Version = pValue->f_String();

		if (auto *pValue = _Params.f_GetMember("AutoUpdateTags"))
		{
			if (pValue->f_IsArray())
			{
				pApplication->m_bAutoUpdate = true;
				for (auto &TagJSON : pValue->f_Array())
				{
					auto &Tag = TagJSON.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
						return DMibErrorInstance(fg_Format("'{}' is not a valid tag", Tag));
					pApplication->m_AutoUpdateTags[Tag];
				}
			}
		}

		if (auto *pValue = _Params.f_GetMember("AutoUpdateBranches"))
		{
			for (auto &BranchJSON : pValue->f_Array())
			{
				auto &Branch = BranchJSON.f_String();
				if (!CVersionManager::fs_IsValidBranch(Branch))
					return DMibErrorInstance(fg_Format("'{}' is not a valid branch", Branch));
				pApplication->m_AutoUpdateBranches[Branch];
			}
		}
		
		auto Directory = pApplication->f_GetDirectory();
		CStr Package = _Params["Package"].f_String();
		
		if (Package.f_IsEmpty())
			return DMibErrorInstance("You have to specify a package");
		
		bool bIsVersionManager = false;
		
		auto *pVersionManagerApplication = mp_VersionManagerApplications.f_FindEqual(Package);

		CVersionManager::CVersionIdentifier VersionID;
		
		if (pVersionManagerApplication)
		{
			bIsVersionManager = true;
			if (!Version.f_IsEmpty())
			{
				CStr Error;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID))
					return DMibErrorInstance(fg_Format("Invalid version format: {}", Error)); 
			}
			else
			{
				auto *pLargest = pVersionManagerApplication->m_VersionsByTime.f_FindLargest();
				if (!pLargest)
					return DMibErrorInstance(fg_Format("No newest version found for application: {}", Package));
				VersionID = pLargest->f_GetVersionID();
			}
		}
		else
		{
			if (!Version.f_IsEmpty())
				return DMibErrorInstance("Package did not refer to any version manager application, so you cannot specify '--version'");
			
			Package = CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory());
			
			if (pApplication->m_Executable.f_IsEmpty())
				return DMibErrorInstance("You must specify an executable to run");
		}
		
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
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (add application): {}", _Error);
			}
		;
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Setup, bForceOverwrite) 
			> [=](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(_Result.f_GetExceptionStr());
					return;
				}
				auto fUnpackApp = [=]
					(CStr const &_SourcePath, CStr const &_DeletePath)
					{
						fLogInfo("Unpacking application");
						fg_Dispatch
							(
								mp_FileActor
								, [=]()
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
									CStr SourcePath = _SourcePath;
									if (CFile::fs_FileExists(_SourcePath, EFileAttrib_Directory))
									{
										auto Files = CFile::fs_FindFiles(_SourcePath + "/*");
										if (Files.f_GetLen() == 1 && Files[0].f_Right(7) == ".tar.gz")
											SourcePath = Files[0];
									}
									TCSet<CStr> AllowExist;
									if (!_DeletePath.f_IsEmpty())
										AllowExist[_DeletePath];
									CStr Output = fsp_UnpackApplication(SourcePath, Directory, pApplication, Files, AllowExist);
									if (!Output.f_IsEmpty())
										pResult->f_AddStdOut(Output);
									
									fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files);											
									
									if (!_DeletePath.f_IsEmpty())
										CFile::fs_DeleteDirectoryRecursive(_DeletePath);
									
									return Files;
								}
							)
							> [=](TCAsyncResult<TCVector<CStr>> &&_Files)
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
								
								mp_Applications[pApplication->m_Name] = pApplication;
								auto InProgressScope = pApplication->f_SetInProgress();

								self(&CAppManagerActor::fp_UpdateApplicationJSON, pApplication) 
									> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
										{
											fLogError(fg_Format("Failed to save state: {}", _Result.f_GetExceptionStr()));
											return;
										}
										fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false) 
											> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
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
				
				if (!bIsVersionManager)
					fUnpackApp(Package, CStr());
				else
				{
					CStr DownloadDirectory = Directory + "/TempVersionDownload";
					fLogInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
					self(&CAppManagerActor::fp_DownloadApplication, Package, VersionID, DownloadDirectory) 
						> [=](TCAsyncResult<CVersionManager::CVersionInformation> &&_VersionInfo)
						{
							if (!_VersionInfo)
							{
								fLogError(fg_Format("Failed to download application from version manager: {}", _VersionInfo.f_GetExceptionStr()));
								return;
							}
							auto &VersionInfo = *_VersionInfo;
							auto &ExtraInfo = VersionInfo.m_ExtraInfo;
							if (pApplication->m_Executable.f_IsEmpty())
							{
								if (auto *pValue = ExtraInfo.f_GetMember("Executable", EJSONType_String))
									pApplication->m_Executable = pValue->f_String();
								if (pApplication->m_Executable.f_IsEmpty())
								{
									Continuation.f_SetException(DMibErrorInstance("No executable was specified and the downloaded version didn't include a default"));
									return;
								}
							}

							if (pApplication->m_RunAsUser.f_IsEmpty())
							{
								if (auto *pValue = ExtraInfo.f_GetMember("RunAsUser", EJSONType_String))
									pApplication->m_RunAsUser = pValue->f_String();
							}

							if (pApplication->m_RunAsGroup.f_IsEmpty())
							{
								if (auto *pValue = ExtraInfo.f_GetMember("RunAsGroup", EJSONType_String))
									pApplication->m_RunAsGroup = pValue->f_String();
							}

							if (pApplication->m_ExecutableParameters.f_IsEmpty())
							{
								if (auto *pValue = ExtraInfo.f_GetMember("ExecutableParams", EJSONType_Array))
								{
									for (auto &Param : pValue->f_Array())
									{
										if (!Param.f_IsString())
											continue;
										
										pApplication->m_ExecutableParameters.f_Insert(Param.f_String());
									}
								}
							}

							pApplication->m_VersionManagerApplication = Package;
							pApplication->m_LastInstalledVersion = VersionID;
							pApplication->m_LastInstalledVersionInfo = VersionInfo;

							fUnpackApp(DownloadDirectory, DownloadDirectory);
						}
					;
				}
			}
		;
		
		return Continuation;
	}
}
