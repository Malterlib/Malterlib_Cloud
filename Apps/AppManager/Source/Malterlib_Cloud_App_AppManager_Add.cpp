// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

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
			ApplicationSettings.m_ExecutableParameters = {"--daemon-run-standalone"};
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

	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_AddApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();
		bool bForceOverwrite = _Params["ForceOverwrite"].f_Boolean();
		bool bForceInstall = _Params["ForceInstall"].f_Boolean();
		
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

		bool bNullPackage = _Params["Package"].f_Type() == EJSONType_Null;
		
		CStr Package;
		if (!bNullPackage)
			Package = _Params["Package"].f_String();
		
		if (Package.f_IsEmpty() && !bNullPackage)
			return DMibErrorInstance("You have to specify a package");
		
		bool bFromFile = _Params["FromFile"].f_Boolean();

		if (bFromFile && bNullPackage)
			return DMibErrorInstance("You cannot specify from file when installing will null package");

		if (Name.f_IsEmpty())
		{
			if (bFromFile || bNullPackage)
				return DMibErrorInstance("You have to specify application name");
			else
				Name = Package;
		}

		bool bSettingsFromVersionInfo = _Params["SettingsFromVersionInfo"].f_Boolean();
		
		CStr Platform;
		TCOptional<CVersionManager::CVersionIDAndPlatform> VersionID;
		
		if (!bFromFile && !bNullPackage)
		{
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
		else if (!bNullPackage)
			Package = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, mp_State.m_RootDirectory));
		
		TCContinuation<uint32> Continuation;

		fp_AddApplication
			(
				Name
				, Settings
				, ChangedSettings
				, bForceOverwrite
				, bForceInstall
				, bSettingsFromVersionInfo
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Add: {}", _Info);
				}
				, bFromFile ? Package : CStr() 
				, VersionID 
			)
			> [=](TCAsyncResult<void> &&_Result)
			{
				Continuation.f_SetResult(_pCommandLine->f_AddAsyncResult(_Result));
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

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissions>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationAdd"}};
		Permissions["App"] = {CPermissions{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty())
			Permissions["Version"] = {{"AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", _Settings.m_VersionManagerApplication)}};

		TCContinuation<void> Continuation;

		mp_Permissions.f_HasPermissions("Add application to AppManager", Permissions)
			> Continuation / [=, fOnInfo = fg_Move(_fOnInfo)](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				if (!_HasPermissions["Command"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Application add, command)"));

				if (!_HasPermissions["App"])
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Application add, app name)"));

				if (auto *pVersion = _HasPermissions.f_FindEqual("Version"))
				{
					if (!*pVersion)
						return Continuation.f_SetException(Auditor.f_AccessDenied("(Application add, version application)"));
				}

				TCSharedPointer<CApplication> pApplication = fg_Construct(_Name, this);

				bool bNullApplication = _FromLocalFile.f_IsEmpty() && _Settings.m_VersionManagerApplication.f_IsEmpty();

				if (!_bSettingsFromVersionInfo)
				{
					CStr Error;
					if (!_Settings.f_Validate(Error))
						return Continuation.f_SetException(Auditor.f_Exception(Error));
				}

				pApplication->m_Settings = _Settings;

				CStr VersionManagerApplication = _Settings.m_VersionManagerApplication;

				CVersionManager::CVersionIDAndPlatform VersionID;

				if (_FromLocalFile.f_IsEmpty() && !bNullApplication)
				{
					auto *pVersionManagerApplication = mp_VersionManagerApplications.f_FindEqual(VersionManagerApplication);

					if (pVersionManagerApplication || _Version)
					{
						CStr Platform = DMalterlibCloudPlatform;
						if (_Version && !_Version->m_Platform.f_IsEmpty())
							Platform = _Version->m_Platform;
						if (!CVersionManager::fs_IsValidPlatform(Platform))
							return Continuation.f_SetException(Auditor.f_Exception("Invalid platform format"));

						if (_Version && _Version->m_VersionID.f_IsValid())
						{
							VersionID.m_Platform = Platform;
							VersionID.m_VersionID = _Version->m_VersionID;
						}
						else
						{
							CStr Error;
							CVersionManager::CVersionInformation VersionInfo;
							VersionID = CAppManagerActor::fp_FindVersion
								(
									pApplication
									, pApplication->m_Settings.m_AutoUpdateTags
									, pApplication->m_Settings.m_AutoUpdateBranches
									, Platform
									, Error
									, EFindVersionFlag_ForAdd
									, VersionInfo
								)
							;

							if (!VersionID.f_IsValid())
								return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No suitable version found for application '{}': {}", VersionManagerApplication, Error)));
						}
					}
					else
					{
						return Continuation.f_SetException
							(
							 	Auditor.f_Exception
								(
									fg_Format
									(
										"No such application '{}' found for connected version managers with known platforms '{vs,vb}'.\n"
										"You might have to specify version and platform manually if a non-default platform is used."
										, VersionManagerApplication
										, mp_KnownPlatforms
									)
								)
							)
						;
					}
				}

				if (pApplication->f_IsChildApp())
				{
					auto *pParentApplication = mp_Applications.f_FindEqual(pApplication->m_Settings.m_ParentApplication);
					if (!pParentApplication)
						return Continuation.f_SetException(Auditor.f_Exception(fg_Format("Parent application '{}' not found", pApplication->m_Settings.m_ParentApplication)));

					if ((*pParentApplication)->f_IsChildApp())
						return Continuation.f_SetException(Auditor.f_Exception("Parent application is not a root application"));
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
						return Continuation.f_SetException(Auditor.f_Exception(fg_Format("Application with name '{}' already exists", pApplication->m_Name)));
				}

				auto Directory = pApplication->f_GetDirectory();

				auto fUnpackAppAndFinish = [=](CStr const &_SourcePath, CStr const &_DeletePath)
					{
#ifdef DPlatformFamily_Windows
						if (!pApplication->m_Settings.m_RunAsUser.f_IsEmpty() && pApplication->m_Settings.m_RunAsUserPassword.f_IsEmpty())
							pApplication->m_Settings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
#endif
						fOnInfo("Unpacking application");
						g_Dispatch(mp_FileActor) >
							[=, pUniqueUserGroup = mp_pUniqueUserGroup]
							{
								auto &Settings = pApplication->m_Settings;
								fsp_CreateApplicationUserGroup(Settings, fOnInfo, Directory / ".home", pUniqueUserGroup);

								TCVector<CStr> Files;

								if (!bNullApplication)
								{
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
									CStr Output = fsp_UnpackApplication
										(
										 	SourcePath
										 	, Directory
										 	, pApplication->m_Name
										 	, pApplication->m_Settings
										 	, Files
										 	, AllowExist
										 	, _bForceInstall
										 	, pUniqueUserGroup
										)
									;
									if (!Output.f_IsEmpty())
										fOnInfo(Output.f_TrimRight());
								}

								fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files, pUniqueUserGroup);

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

								pApplication->m_LastInstalledVersionFinished = pApplication->m_LastInstalledVersion;
								pApplication->m_LastInstalledVersionInfoFinished = pApplication->m_LastInstalledVersionInfo;

								fp_UpdateApplicationJSON(pApplication)
									> Continuation % "Failed to save state" % Auditor / [=]
									{
										pApplication->m_bJustUpdated = true;
										fp_LaunchApp(pApplication, false)
											> Continuation % "Failed to launch app. Will retry periodically" % Auditor / [=, InProgressScope = InProgressScope](bool _bQuitManager)
											{
												fOnInfo("Application was successfully added");
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
						auto fApplyVersion = [=](CVersionManager::CVersionIDAndPlatform const &_VersionID, CVersionManager::CVersionInformation const &_VersionInfo)
							{
								if (_bSettingsFromVersionInfo)
								{
									CApplicationSettings NewSettings = pApplication->m_Settings;
									CApplicationSettings VersionInfoSettings;
									EApplicationSetting NewChangedSettings = EApplicationSetting_None;
									try
									{
										VersionInfoSettings.f_FromVersionInfo(_VersionInfo, NewChangedSettings);
									}
									catch (CException const &_Exception)
									{
										DMibError(fg_Format("Failed to get settings from version info: {}", _Exception));
									}
									NewSettings.f_ApplySettings(NewChangedSettings, VersionInfoSettings);

									CStr Error;
									if (!NewSettings.f_Validate(Error))
										DMibError(Error);

									pApplication->m_Settings = NewSettings;
								}

								pApplication->m_LastInstalledVersion = _VersionID;
								pApplication->m_LastInstalledVersionInfo = _VersionInfo;
								if (mp_KnownPlatforms(_VersionID.m_Platform).f_WasCreated())
									fp_VersionManagerResubscribeAll();
							}
						;

						if (!_FromLocalFile.f_IsEmpty() || bNullApplication)
						{
							if (_bSettingsFromVersionInfo && _FromLocalFile.f_EndsWith(".tar.gz"))
							{
								auto &LaunchActor = mp_LaunchActors.f_Insert() = fg_Construct();

								CProcessLaunchActor::CSimpleLaunch Launch
									{
						#ifdef DPlatformFamily_Windows
										"tar.exe"
						#else
										"tar"
						#endif
										,
										{
											"-xOf"
						#ifdef DPlatformFamily_Windows
											, NFile::NPlatform::fg_ConvertToMinGWPath(_FromLocalFile)
						#else
											, _FromLocalFile
						#endif
											, "VersionInfo.json"
										}
										, CFile::fs_GetPath(_FromLocalFile)
										, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
									}
								;

								LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch))
									> [=](TCAsyncResult<CProcessLaunchActor::CSimpleLaunchResult> &&_LaunchResult)
									{
										if (_LaunchResult)
										{
											try
											{
												CEJSON VersionInfoJSON = CEJSON::fs_FromString(_LaunchResult->f_GetStdOut());

												CStr Application;
												CStr Version;
												CStr Configuration;
												CStr Platform;
												CEJSON ExtraInfo;

												auto fApplySettings = [&](CEJSON const &_Settings)
													{
														if (auto *pValue = _Settings.f_GetMember("Application", EJSONType_String))
															Application = pValue->f_String();
														if (auto *pValue = _Settings.f_GetMember("Version", EJSONType_String))
															Version = pValue->f_String();
														if (auto *pValue = _Settings.f_GetMember("Configuration", EJSONType_String))
															Configuration = pValue->f_String();
														if (auto *pValue = _Settings.f_GetMember("Platform", EJSONType_String))
															Platform = pValue->f_String();
														if (auto *pValue = _Settings.f_GetMember("ExtraInfo", EJSONType_Object))
															ExtraInfo = pValue->f_Object();
													}
												;

												fApplySettings(VersionInfoJSON);

												if (Application.f_IsEmpty())
													DMibError("Application must be specified");
												if (Version.f_IsEmpty())
													DMibError("Version must be specified");

												if (!CVersionManager::fs_IsValidApplicationName(Application))
													DMibError("Application format is invalid");

												CVersionManager::CVersionIDAndPlatform VersionID;
												{
													CStr Error;
													if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &VersionID.m_VersionID))
														DMibError(fg_Format("Version identifier format is invalid: {}", Error));
												}

												if (!CVersionManager::fs_IsValidPlatform(Platform))
													DMibError("Invalid version platform format");

												VersionID.m_Platform = Platform;

												CVersionManager::CVersionInformation VersionInfo;
												VersionInfo.m_Configuration = Configuration;
												VersionInfo.m_ExtraInfo = ExtraInfo;

												fApplyVersion(VersionID, VersionInfo);

												pApplication->m_Settings.m_VersionManagerApplication = Application;
											}
											catch (CException const &_Exception)
											{
												fOnInfo("Failed to parse version info from VersionInfo.json in package: {}"_f << _Exception);
											}
										}
										else
											fOnInfo("Failed to extract version info from package: {}"_f << _LaunchResult.f_GetExceptionStr());

										fUnpackAppAndFinish(_FromLocalFile, CStr());
									}
								;
							}
							else
								fUnpackAppAndFinish(_FromLocalFile, CStr());
							return;
						}

						CStr DownloadDirectory = Directory + "/TempVersionDownload";
						fOnInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
						self(&CAppManagerActor::fp_DownloadApplication, VersionManagerApplication, VersionID, DownloadDirectory)
							> Continuation % "Failed to download application from version manager" % Auditor / [=](CVersionManager::CVersionInformation &&_VersionInfo)
							{
								auto &VersionInfo = _VersionInfo;

								try
								{
									fApplyVersion(VersionID, VersionInfo);
								}
								catch (CException const &_Exception)
								{
									Continuation.f_SetException(Auditor.f_Exception(_Exception.f_GetErrorStr()));
								}

								fUnpackAppAndFinish(DownloadDirectory, DownloadDirectory);
							}
						;
					}
				;
			}
		;

		return Continuation;
	}
}
