// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/AsyncDestroy>

#include "Malterlib_Cloud_App_AppManager.h"

#ifdef DPlatformFamily_Windows
#include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Add
		(
			NStr::CStr const &_Name
			, CApplicationAdd const &_Add
			, CApplicationSettings const &_Settings
		)
	{
		NConcurrency::TCPromise<void> Promise;

		CAppManagerActor::CApplicationSettings ApplicationSettings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		ApplicationSettings.f_FromInterfaceAdd(_Add, ChangedSettings);
		ApplicationSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);

		if (!_Settings.m_ExecutableParameters)
		{
			ApplicationSettings.m_ExecutableParameters = {"--daemon-run-standalone"};
			ChangedSettings |= EApplicationSetting_ExecutableParameters;
		}

		return Promise <<= m_pThis->self
			(
				&CAppManagerActor::fp_AddApplication
				, _Name
				, ApplicationSettings
				, ChangedSettings
				, _Add.m_bForceOverwriteEncryption
				, _Add.m_bForceInstall
				, _Add.m_bSettingsFromVersionInfo
				, [](CStr const &_Info) 
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Add: {}", _Info);
				}
				, CStr()
				, _Add.m_Version
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_AddApplication(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CallingHostInfo = fg_GetCallingHostInfo();
		CStr Name = _Params["Name"].f_String();
		bool bForceOverwrite = _Params["ForceOverwrite"].f_Boolean();
		bool bForceInstall = _Params["ForceInstall"].f_Boolean();
		
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		CApplicationSettings Settings;
		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error, true))
				co_return DMibErrorInstance(Error);
		}

		CStr Version;
		if (auto *pValue = _Params.f_GetMember("Version"))
			Version = pValue->f_String();

		bool bNullPackage = _Params["Package"].f_Type() == EJSONType_Null;
		
		CStr Package;
		if (!bNullPackage)
			Package = _Params["Package"].f_String();
		
		if (Package.f_IsEmpty() && !bNullPackage)
			co_return DMibErrorInstance("You have to specify a package");
		
		bool bFromFile = _Params["FromFile"].f_Boolean();

		if (bFromFile && bNullPackage)
			co_return DMibErrorInstance("You cannot specify from file when installing will null package");

		if (Name.f_IsEmpty())
		{
			if (bFromFile || bNullPackage)
				co_return DMibErrorInstance("You have to specify application name");
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
					co_return DMibErrorInstance(fg_Format("Invalid version format: {}", Error));
				VersionID = VersionIDTemp;
			}
			Settings.m_VersionManagerApplication = Package;
		}
		else if (!bNullPackage)
			Package = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, mp_State.m_RootDirectory));
		
		auto Result = co_await self
			(
				&CAppManagerActor::fp_AddApplication
				, Name
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
				, CallingHostInfo
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}
	
	TCFuture<void> CAppManagerActor::fp_AddApplication
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
			, CCallingHostInfo const &_CallingHostInfo
		)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationAdd"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty())
			Permissions["Version"] = {{"AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", _Settings.m_VersionManagerApplication)}};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(mp_Permissions.f_HasPermissions("Add application to AppManager", Permissions, _CallingHostInfo) % "Permission denied adding application" % Auditor)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application add, command)", Permissions["Command"]);

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application add, app name)", Permissions["App"]);

		if (auto *pVersion = HasPermissions.f_FindEqual("Version"))
		{
			if (!*pVersion)
				co_return Auditor.f_AccessDenied("(Application add, version application)", Permissions["Version"]);
		}

		TCSharedPointer<CApplication> pApplication = fg_Construct(_Name, this);

		bool bNullApplication = _FromLocalFile.f_IsEmpty() && _Settings.m_VersionManagerApplication.f_IsEmpty();

		if (!_bSettingsFromVersionInfo)
		{
			CStr Error;
			if (!_Settings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
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
					co_return Auditor.f_Exception("Invalid platform format");

				if (_Version && _Version->m_VersionID.f_IsValid())
				{
					VersionID.m_Platform = Platform;
					VersionID.m_VersionID = _Version->m_VersionID;
				}
				else
				{
					if (mp_KnownPlatforms(Platform).f_WasCreated())
						co_await fp_VersionManagerResubscribeAll();
					CStr Error;
					CVersionManager::CVersionInformation VersionInfo;
					bool bNewestUnconditionalVersionChanged = false;
					VersionID = CAppManagerActor::fp_FindVersion
						(
							pApplication
							, pApplication->m_Settings.m_UpdateTags
							, pApplication->m_Settings.m_UpdateBranches
							, Platform
							, Error
							, EFindVersionFlag_ForAdd
							, VersionInfo
							, pApplication->m_NewestUnconditionalVersion
							, pApplication->m_NewestUnconditionalVersionInfo
							, bNewestUnconditionalVersionChanged
						)
					;

					if (!VersionID.f_IsValid())
						co_return Auditor.f_Exception(fg_Format("No suitable version found for application '{}': {}", VersionManagerApplication, Error));
				}
			}
			else
			{
				co_return Auditor.f_Exception
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
				co_return Auditor.f_Exception(fg_Format("Parent application '{}' not found", pApplication->m_Settings.m_ParentApplication));

			if ((*pParentApplication)->f_IsChildApp())
				co_return Auditor.f_Exception("Parent application is not a root application");
			pApplication->m_pParentApplication = &**pParentApplication;
			pApplication->m_pParentApplication->m_Children.f_Insert(*pApplication);
		}

		auto pCleanup = g_OnScopeExitActor / [pApplication]
			{
				pApplication->f_Delete();
			}
		;

		if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
		{
			if (pApplicationsState->f_GetMember(pApplication->m_Name))
				co_return Auditor.f_Exception(fg_Format("Application with name '{}' already exists", pApplication->m_Name));
		}

		auto Directory = pApplication->f_GetDirectory();

		co_await (self(&CAppManagerActor::fp_ChangeEncryption, pApplication, EEncryptOperation_Setup, _bForceOverwrite) % Auditor);

		auto fApplyVersion = [&](CVersionManager::CVersionIDAndPlatform const &_VersionID, CVersionManager::CVersionInformation const &_VersionInfo)
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
				pApplication->m_LastTriedInstalledVersion = _VersionID;
				pApplication->m_LastTriedInstalledVersionInfo = _VersionInfo;

				if (mp_KnownPlatforms(_VersionID.m_Platform).f_WasCreated())
					fp_VersionManagerResubscribeAll() > fg_DiscardResult();
			}
		;

		TCSharedPointer<CStr> pDeletePath = fg_Construct();
		CStr SourcePath;

		auto CleanupDownload = g_BlockingActorSubscription / [pDeletePath]
			{
				if (!*pDeletePath)
					return;

				try
				{
					if (CFile::fs_FileExists(*pDeletePath))
						CFile::fs_DeleteDirectoryRecursive(*pDeletePath);
				}
				catch (CExceptionFile const &_Exception)
				{
					[[maybe_unused]] auto &Exception = _Exception;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to clean up version download: {}", Exception);
				}
			}
		;

		if (!_FromLocalFile.f_IsEmpty() || bNullApplication)
		{
			if (_bSettingsFromVersionInfo && (_FromLocalFile.f_EndsWith(".tar.gz") || _FromLocalFile.f_EndsWith(".tar")))
			{
				auto &LaunchActor = mp_LaunchActors.f_Insert() = fg_Construct();

				CProcessLaunchActor::CSimpleLaunch Launch
					{
						mp_State.m_RootDirectory / "bin/bsdtar"
						,
						{
							"-xqOf"
							, _FromLocalFile
							, "*VersionInfo.json"
						}
						, CFile::fs_GetPath(_FromLocalFile)
						, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
					}
				;

				auto LaunchResult = co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch)).f_Wrap();

				if (LaunchResult)
				{
					try
					{
						CEJSONSorted VersionInfoJSON = CEJSONSorted::fs_FromString(LaunchResult->f_GetStdOut());

						CStr Application;
						CStr Version;
						CStr Configuration;
						CStr Platform;
						CEJSONSorted ExtraInfo;

						auto fApplySettings = [&](CEJSONSorted const &_Settings)
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
						_fOnInfo("Failed to parse version info from VersionInfo.json in package: {}"_f << _Exception);
					}
				}
				else
					_fOnInfo("Failed to extract version info from package: {}"_f << LaunchResult.f_GetExceptionStr());

				SourcePath = _FromLocalFile;
			}
			else
				SourcePath = _FromLocalFile;
		}
		else
		{
			CStr DownloadDirectoryRoot = Directory / "TempVersionDownload";
			CStr DownloadDirectory = DownloadDirectoryRoot / fg_RandomID();
			_fOnInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
			auto VersionInfo = co_await
				(
					self(&CAppManagerActor::fp_DownloadApplication, VersionManagerApplication, VersionID, DownloadDirectory)
					% "Failed to download application from version manager" % Auditor
				)
			;
			{
				auto CaptureScope = co_await (g_CaptureExceptions % Auditor);
				fApplyVersion(VersionID, VersionInfo);
			}

			*pDeletePath = DownloadDirectoryRoot;
			SourcePath = DownloadDirectory;
		}

#ifdef DPlatformFamily_Windows
		if (!pApplication->m_Settings.m_RunAsUser.f_IsEmpty() && pApplication->m_Settings.m_RunAsUserPassword.f_IsEmpty())
			pApplication->m_Settings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
#endif
		_fOnInfo("Unpacking application");

		TCVector<CStr> Files;
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			Files = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [=, pUniqueUserGroup = mp_pUniqueUserGroup, RootDirectory = mp_State.m_RootDirectory]() mutable
					{
						auto &Settings = pApplication->m_Settings;
						fsp_CreateApplicationUserGroup(Settings, _fOnInfo, Directory / ".home", pUniqueUserGroup);

						TCVector<CStr> Files;

						if (!bNullApplication)
						{
							if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
							{
								auto Files = CFile::fs_FindFiles(SourcePath + "/*");
								if (Files.f_GetLen() == 1 && (Files[0].f_EndsWith(".tar.gz") || Files[0].f_EndsWith(".tar")))
									SourcePath = Files[0];
							}
							TCSet<CStr> AllowExist;
							AllowExist[Directory + "/lost+found"];
							AllowExist[Directory + "/.home"];
							AllowExist[Directory + "/.tmp"];
							if (!pDeletePath->f_IsEmpty())
								AllowExist[*pDeletePath];
							CStr Output = fsp_UnpackApplication
								(
									RootDirectory
									, SourcePath
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
								_fOnInfo(Output.f_TrimRight());
						}

						fsp_UpdateApplicationFilePermissions(Directory, pApplication, pApplication->m_Files, pUniqueUserGroup, _fOnInfo);

						return Files;
					}
					% "Failed to unpack application" % Auditor
				)
			;
		}

		if (auto *pApplicationsState = mp_State.m_StateDatabase.m_Data.f_GetMember("Applications"))
		{
			if (pApplicationsState->f_GetMember(pApplication->m_Name))
				co_return Auditor.f_Exception(fg_Format("Application with name '{}' already exists", pApplication->m_Name));
		}

		if (CleanupDownload)
			co_await CleanupDownload->f_Destroy();

		pApplication->m_Files = fg_Move(Files);

		mp_Applications[pApplication->m_Name] = pApplication;
		pCleanup->f_Clear();

		fp_OnApplicationAdded(pApplication);
		auto InProgressScope = pApplication->f_SetInProgress("Add Application");
		auto DestroyInProgress = co_await fg_AsyncDestroy(fg_Move(InProgressScope));

		pApplication->m_LastInstalledVersionFinished = pApplication->m_LastInstalledVersion;
		pApplication->m_LastInstalledVersionInfoFinished = pApplication->m_LastInstalledVersionInfo;

		co_await (fp_UpdateApplicationJSON(pApplication) % "Failed to save state" % Auditor);

		pApplication->m_bJustUpdated = true;
		CAppLaunchResult AppLaunchResult = co_await (fp_LaunchApp(pApplication, false) % "Failed to launch app. Will retry periodically" % Auditor);

		_fOnInfo("Application was successfully added");
		Auditor.f_Info("Application added");
		if (AppLaunchResult.m_StartupError)
			_fOnInfo("Application startup failed: {}"_f << AppLaunchResult.m_StartupError);

		co_await fg_Move(DestroyInProgress);
		co_await fp_SyncNotifications(_Name);

		co_return {};
	}
}
