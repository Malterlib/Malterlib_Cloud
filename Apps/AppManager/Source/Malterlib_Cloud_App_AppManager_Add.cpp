// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCContinuation<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Add
		(
			NStr::CStr const &_Name
			, CApplicationAdd const &_Add
			, CApplicationSettings const &_Settings
		)
	{
		CAppManagerActor::CApplicationSettings ApplicationSettings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		ApplicationSettings.f_FromInterfaceAdd(_Add, ChangedSettings);
		ApplicationSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);
		if (!_Settings.m_ExecutableParameters)
		{
			ApplicationSettings.m_ExecutableParameters = {"--daemon-run"};
			ChangedSettings |= EApplicationSetting_ExecutableParameters;
		}
		return m_pThis->fp_AddApplication
			(
				_Name
				, ApplicationSettings
				, ChangedSettings
				, _Add.m_bForceOverwriteEncryption
				, _Add.m_bForceInstall
				, _Add.m_bSettingsFromVersionInfo
				, [](CStr const &_Info) 
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Add: {}", _Info);
				}
				, {}
				, _Add.m_Version 
			)
		;
	}

	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_AddApplication(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		bool bForceOverwrite = _Params["ForceOverwrite"].f_Boolean();
		bool bForceInstall = _Params["ForceInstall"].f_Boolean();
		
		if (Name.f_IsEmpty())
			return DMibErrorInstance("You have to specify application name");
		
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		CApplicationSettings Settings;
		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error, true))
				return DMibErrorInstance(Error);
		}

		CStr Version;
		if (auto *pValue = _Params.f_GetMember("Version"))
			Version = pValue->f_String();

		CStr Package = _Params["Package"].f_String();
		
		if (Package.f_IsEmpty())
			return DMibErrorInstance("You have to specify a package");
		
		bool bFromFile = _Params["FromFile"].f_Boolean();
		
		bool bSettingsFromVersionInfo = false;
		
		CStr Platform;
		TCOptional<CVersionManager::CVersionIDAndPlatform> VersionID;
		
		if (!bFromFile)
		{
			bSettingsFromVersionInfo = _Params["SettingsFromVersionInfo"].f_Boolean();
			CVersionManager::CVersionIDAndPlatform VersionIDTemp;
			if (auto *pValue = _Params.f_GetMember("VersionManagerPlatform"))
			{
				VersionIDTemp.m_Platform = pValue->f_String();
				VersionID = VersionIDTemp;
			}
			if (!Version.f_IsEmpty())
			{
				CStr Error;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionIDTemp.m_VersionID))
					return DMibErrorInstance(fg_Format("Invalid version format: {}", Error));
				VersionID = VersionIDTemp;
			}
			Settings.m_VersionManagerApplication = Package;
		}
		else
			Package = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, CFile::fs_GetProgramDirectory()));

		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		
		fp_AddApplication
			(
				Name
				, Settings
				, ChangedSettings
				, bForceOverwrite
				, bForceInstall
				, bSettingsFromVersionInfo
				, [pResult](CStr const &_Info)
				{
					pResult->f_AddStdOut(_Info + DMibNewLine);
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Add: {}", _Info);
				}
				, bFromFile ? Package : CStr() 
				, VersionID 
			)
			> [=](TCAsyncResult<void> &&_Result)
			{
				pResult->f_AddAsyncResult(_Result);
				Continuation.f_SetResult(fg_Move(*pResult));
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_AddApplication
		(
			NStr::CStr const &_Name
			, CApplicationSettings const &_Settings
			, EApplicationSetting _ChangedSettings
			, bool _bForceOverwrite
			, bool _bForceInstall
			, bool _bSettingsFromVersionInfo
			, TCFunction<void (CStr const &_Info)> &&_fOnInfo
			, CStr const &_FromLocalFile
			, TCOptional<CVersionManager::CVersionIDAndPlatform> const &_Version 
		)
	{
		auto Auditor = f_Auditor();
		auto CallingHostID = fg_GetCallingHostID();

		if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationAdd"))
			return Auditor.f_AccessDenied("(Application add, command)");

		if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)))
			return Auditor.f_AccessDenied("(Application add, app name)");
		
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty())
		{
			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", _Settings.m_VersionManagerApplication)))
				return Auditor.f_AccessDenied("(Application add, version application)");
		}
		
		TCSharedPointer<CApplication> pApplication = fg_Construct(_Name, this);
		
		if (_FromLocalFile.f_IsEmpty() && _Settings.m_VersionManagerApplication.f_IsEmpty())
			return Auditor.f_Exception("You have to specify version manager application");

		if (!_bSettingsFromVersionInfo)
		{
			CStr Error;
			if (!_Settings.f_Validate(Error))
				return Auditor.f_Exception(Error);
		}
		
		pApplication->m_Settings = _Settings;
		
		CStr VersionManagerApplication = _Settings.m_VersionManagerApplication;
		
		CVersionManager::CVersionIDAndPlatform VersionID;
		
		if (_FromLocalFile.f_IsEmpty())
		{
			auto *pVersionManagerApplication = mp_VersionManagerApplications.f_FindEqual(VersionManagerApplication);

			if (pVersionManagerApplication || _Version)
			{
				CStr Platform = DMalterlibCloudPlatform;
				if (_Version && !_Version->m_Platform.f_IsEmpty())
					Platform = _Version->m_Platform;
				if (!CVersionManager::fs_IsValidPlatform(Platform))
					return Auditor.f_Exception("Invalid platform format"); 

				if (_Version && _Version->m_VersionID.f_IsValid())
				{
					VersionID.m_Platform = Platform;
					VersionID.m_VersionID = _Version->m_VersionID;
				}
				else
				{
					CStr Error;
					VersionID = CAppManagerActor::fp_FindVersion
						(
							pApplication
							, pApplication->m_Settings.m_AutoUpdateTags
							, pApplication->m_Settings.m_AutoUpdateBranches
							, Platform 
							, Error
							, EFindVersionFlag_ForAdd
						)
					;
					
					if (!VersionID.f_IsValid())
						return Auditor.f_Exception(fg_Format("No suitable version found for application '{}': {}", VersionManagerApplication, Error));
				}
			}
			else
			{
				return Auditor.f_Exception
					(
						fg_Format
						(
							"No such application '{}' found for connected version managers with known platforms '{vs,vb}'.\n"
							"You might have to specify version and platform manually if a non-default platform is used."
							, VersionManagerApplication
							, mp_KnownPlatforms 
						)
					)
				;
			}
		}
		
		if (pApplication->f_IsChildApp())
		{
			auto *pParentApplication = mp_Applications.f_FindEqual(pApplication->m_Settings.m_ParentApplication);
			if (!pParentApplication)
				return Auditor.f_Exception(fg_Format("Parent application '{}' not found", pApplication->m_Settings.m_ParentApplication));
			
			if ((*pParentApplication)->f_IsChildApp())			
				return Auditor.f_Exception("Parent application is not a root application");
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
				return Auditor.f_Exception(fg_Format("Application with name '{}' already exists", pApplication->m_Name));
		}
		
		TCContinuation<void> Continuation;
		
		auto Directory = pApplication->f_GetDirectory();

		auto fUnpackAppAndFinish = [=](CStr const &_SourcePath, CStr const &_DeletePath)
			{
				_fOnInfo("Unpacking application");
				g_Dispatch(mp_FileActor) > 
					[=]
					{
						auto &Settings = pApplication->m_Settings;
						fsp_CreateApplicationUserGroup(Settings, _fOnInfo, Directory);

						TCVector<CStr> Files;
						CStr SourcePath = _SourcePath;
						if (CFile::fs_FileExists(_SourcePath, EFileAttrib_Directory))
						{
							auto Files = CFile::fs_FindFiles(_SourcePath + "/*");
							if (Files.f_GetLen() == 1 && Files[0].f_Right(7) == ".tar.gz")
								SourcePath = Files[0];
						}
						TCSet<CStr> AllowExist;
						AllowExist[Directory + "/lost+found"];
						if (!_DeletePath.f_IsEmpty())
							AllowExist[_DeletePath];
						CStr Output = fsp_UnpackApplication(SourcePath, Directory, pApplication->m_Name, pApplication->m_Settings, Files, AllowExist, _bForceInstall);
						if (!Output.f_IsEmpty())
							_fOnInfo(Output.f_TrimRight());
						
						fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files);											
						
						if (!_DeletePath.f_IsEmpty())
							CFile::fs_DeleteDirectoryRecursive(_DeletePath);
						
						return Files;
					}
					> Continuation % "Failed to unpack application" % Auditor / [=](TCVector<CStr> &&_Files)
					{
						if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
						{
							if (pApplicationsState->f_GetMember(pApplication->m_Name))
							{
								Continuation.f_SetException(Auditor.f_Exception(fg_Format("Application with name '{}' already exists", pApplication->m_Name)));
								return;
							}
						}

						pApplication->m_Files = fg_Move(_Files);
						
						mp_Applications[pApplication->m_Name] = pApplication;
						pCleanup->f_Clear();
						
						fp_OnApplicationAdded(pApplication);
						auto InProgressScope = pApplication->f_SetInProgress();

						fp_UpdateApplicationJSON(pApplication) 
							> Continuation % "Failed to save state" % Auditor / [=]
							{
								pApplication->m_bJustUpdated = true;
								fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false) 
									> Continuation % "Failed to launch app. Will retry periodically" % Auditor / [=, InProgressScope = InProgressScope]
									{
										_fOnInfo("Application was successfully added");
										Auditor.f_Info("Application added");
										Continuation.f_SetResult();
									}
								;
							}
						;
					}
				;
			}
		;
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Setup, _bForceOverwrite) 
			> Continuation % Auditor / [=]
			{
				if (!_FromLocalFile.f_IsEmpty())
					fUnpackAppAndFinish(_FromLocalFile, CStr());
				else
				{
					CStr DownloadDirectory = Directory + "/TempVersionDownload";
					_fOnInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
					self(&CAppManagerActor::fp_DownloadApplication, VersionManagerApplication, VersionID, DownloadDirectory) 
						> Continuation % "Failed to download application from version manager" % Auditor / [=](CVersionManager::CVersionInformation &&_VersionInfo)
						{
							auto &VersionInfo = _VersionInfo;
							
							if (_bSettingsFromVersionInfo)
							{
								CApplicationSettings NewSettings = pApplication->m_Settings;
								CApplicationSettings VersionInfoSettings;
								EApplicationSetting NewChangedSettings = EApplicationSetting_None;
								VersionInfoSettings.f_FromVersionInfo(VersionInfo, NewChangedSettings);
								NewSettings.f_ApplySettings(NewChangedSettings, VersionInfoSettings);
								
								CStr Error;
								if (!NewSettings.f_Validate(Error))
								{
									Continuation.f_SetException(Auditor.f_Exception(Error));
									return ;
								}
								pApplication->m_Settings = NewSettings;
							}

							pApplication->m_LastInstalledVersion = VersionID;
							pApplication->m_LastInstalledVersionInfo = VersionInfo;
							if (mp_KnownPlatforms(VersionID.m_Platform).f_WasCreated())
								fp_VersionManagerResubscribeAll();

							fUnpackAppAndFinish(DownloadDirectory, DownloadDirectory);
						}
					;
				}
			}
		;
		
		return Continuation;
	}
}
