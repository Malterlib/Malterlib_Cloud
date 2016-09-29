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
		bool bForceInstall = _Params["ForceInstall"].f_Boolean();
		
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		{
			CStr Error;
			if (!pApplication->m_Settings.f_ParseSettings(_Params, ChangedSettings, Error, true))
				return DMibErrorInstance(Error);
		}

		CStr Version;
		if (auto *pValue = _Params.f_GetMember("Version"))
			Version = pValue->f_String();

		auto Directory = pApplication->f_GetDirectory();
		CStr Package = _Params["Package"].f_String();
		
		if (Package.f_IsEmpty())
			return DMibErrorInstance("You have to specify a package");
		
		bool bIsVersionManager = false;
		bool bSettingsFromVersionInfo = false;
		
		CStr Platform;
		
		auto *pVersionManagerApplication = mp_VersionManagerApplications.f_FindEqual(Package);

		CVersionManager::CVersionIDAndPlatform VersionID;
		
		if (pVersionManagerApplication || !Version.f_IsEmpty())
		{
			Platform = DMalterlibCloudPlatform;
			if (auto *pValue = _Params.f_GetMember("VersionManagerPlatform"))
				Platform = pValue->f_String();
			bSettingsFromVersionInfo = _Params["SettingsFromVersionInfo"].f_Boolean();
			bIsVersionManager = true;
			if (!CVersionManager::fs_IsValidPlatform(Platform))
				return DMibErrorInstance("Invalid platform format"); 
			
			if (!Version.f_IsEmpty())
			{
				CStr Error;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
					return DMibErrorInstance(fg_Format("Invalid version format: {}", Error));
				VersionID.m_Platform = Platform;
			}
			else
			{
				decltype(pVersionManagerApplication->m_VersionsByTime.f_GetIterator()) iVersion;
				for (iVersion.f_StartBackward(pVersionManagerApplication->m_VersionsByTime); iVersion; --iVersion)
				{
					auto &Version = *iVersion;
					if (Version.f_GetVersionID().m_Platform != Platform)
						continue;
					VersionID = Version.f_GetVersionID(); 
					break;
				}
				if (!iVersion)
					return DMibErrorInstance(fg_Format("No newest version found for application: {}", Package));
			}
		}
		else
		{
			if (!Version.f_IsEmpty())
				return DMibErrorInstance("Package did not refer to any version manager application, so you cannot specify '--version'");
			if (!Platform.f_IsEmpty())
				return DMibErrorInstance("Package did not refer to any version manager application, so you cannot specify '--platform'");
			
			Package = CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory());
		}

		if (!bSettingsFromVersionInfo)
		{
			CStr Error;
			if (!pApplication->m_Settings.f_Validate(Error))
				return DMibErrorInstance(Error);
		}
		
		if (pApplication->f_IsChildApp())
		{
			auto *pParentApplication = mp_Applications.f_FindEqual(pApplication->m_Settings.m_ParentApplication);
			if (!pParentApplication)
				return DMibErrorInstance(fg_Format("Parent application '{}' not found", pApplication->m_Settings.m_ParentApplication));
			
			if ((*pParentApplication)->f_IsChildApp())			
				return DMibErrorInstance("Parent application is not a root application");
			pApplication->m_pParentApplication = &**pParentApplication;
			pApplication->m_pParentApplication->m_Children.f_Insert(*pApplication);
		}
		auto pCleanup = g_OnScopeExitActor > [pApplication]
			{
				pApplication->f_Delete();
			}
		;
		
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
									auto &Settings = pApplication->m_Settings;
									fsp_CreateApplicationUserGroup(Settings, fLogInfo, Directory);

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
									CStr Output = fsp_UnpackApplication(SourcePath, Directory, pApplication, Files, AllowExist, bForceInstall);
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
								pCleanup->f_Clear();
								
								fp_ApplicationCreated(pApplication);
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
							
							if (bSettingsFromVersionInfo)
							{
								CApplicationSettings NewSettings;
								EApplicationSetting NewChangedSettings = EApplicationSetting_None;
								NewSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
								NewSettings.f_ApplySettings(ChangedSettings, pApplication->m_Settings);
								
								CStr Error;
								if (!NewSettings.f_Validate(Error))
								{
									Continuation.f_SetException(DMibErrorInstance(Error));
									return ;
								}
								pApplication->m_Settings = NewSettings;
							}

							pApplication->m_Settings.m_VersionManagerApplication = Package;
							
							pApplication->m_LastInstalledVersion = VersionID;
							pApplication->m_LastInstalledVersionInfo = VersionInfo;
							if (mp_KnownPlatforms(VersionID.m_Platform).f_WasCreated())
								fp_VersionManagerResubscribeAll();

							fUnpackApp(DownloadDirectory, DownloadDirectory);
						}
					;
				}
			}
		;
		
		return Continuation;
	}
}
