// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/AsyncDestroy>

#include "Malterlib_Cloud_App_AppManager.h"

#ifdef DPlatformFamily_Windows
#	include <Mib/Core/PlatformSpecific/WindowsFilePath>
#endif

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Add(NStr::CStr _Name, CApplicationAdd _Add, CApplicationSettings _Settings)
	{
		CAppManagerActor::CApplicationSettings ApplicationSettings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		ApplicationSettings.f_FromInterfaceAdd(_Add, ChangedSettings);
		ApplicationSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);

		if (!_Settings.m_ExecutableParameters)
			ApplicationSettings.m_ExecutableParameters = {"--daemon-run-standalone"};

		co_return co_await m_pThis->fp_AddApplication
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
				, CStr()
				, _Add.m_Version
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_EnableSelfUpdate(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		bool bFromFile = _Params["FromFile"].f_Boolean();
		auto CallingHostInfo = fg_GetCallingHostInfo();

		// The package metadata is read from host paths the caller names before the additions check their own permissions, so the caller needs
		// access to the applications that read them. The platform of the self update package is only known once it is read; its addition checks
		// the access to the agent application it then installs as
		{
			auto Auditor = f_Auditor({}, CallingHostInfo);

			NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
			Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationAdd"}};

			auto fAddApplicationPermission = [&](CStr const &_Name)
				{
					Permissions[CStr("App/{}"_f << _Name)] =
						{
							CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)
						}
					;
				}
			;

			if (bFromFile && !_Params["Package"].f_AsString().f_IsEmpty())
				fAddApplicationPermission(_Params["Name"].f_String());

			if (auto *pPlatforms = _Params.f_GetMember("AgentPlatforms"))
			{
				for (auto &Platform : pPlatforms->f_Array())
				{
					CStr const &Entry = Platform.f_String();
					auto iEquals = Entry.f_Find("=");
					if (iEquals >= 0)
						fAddApplicationPermission("SelfUpdate.{}"_f << Entry.f_Left(iEquals));
				}
			}

			NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
				(
					mp_Permissions.f_HasPermissions("Enable self update in AppManager", Permissions, CallingHostInfo) % "Permission denied enabling self update" % Auditor
				)
			;

			for (auto &bHasPermission : HasPermissions)
			{
				if (!bHasPermission)
				{
					auto &Key = HasPermissions.fs_GetKey(bHasPermission);
					co_return Auditor.f_AccessDenied(CStr("(Enable self update, {})"_f << Key), Permissions[Key]);
				}
			}
		}

		CStr DetectedPlatform;
		if (bFromFile)
		{
			CStr Package = _Params["Package"].f_AsString();
			if (!Package.f_IsEmpty())
			{
				CStr FromLocalFile = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, mp_State.m_RootDirectory));

				// Without readable platform metadata, the package is the host self update source, as before agent distributions
				// existed: the operator chooses the package, and packages are trusted
				auto PackageInfo = co_await fp_ReadPackageVersionInfo
					(
						FromLocalFile
						, [](CStr const &_Info)
						{
						}
					)
					.f_Wrap()
				;

				if (PackageInfo && *PackageInfo)
					DetectedPlatform = (**PackageInfo).m_VersionID.m_Platform;
			}
		}

		bool bPackageIsAgent = bFromFile && !DetectedPlatform.f_IsEmpty() && DetectedPlatform != DMalterlibCloudPlatform;

		struct CAgentPlatform
		{
			CStr m_Platform;
			CStr m_LocalPath;
		};

		TCVector<CAgentPlatform> AgentPlatforms;
		if (auto *pPlatforms = _Params.f_GetMember("AgentPlatforms"))
		{
			for (auto &Platform : pPlatforms->f_Array())
			{
				CAgentPlatform AgentPlatform;
				AgentPlatform.m_Platform = Platform.f_String();

				auto iEquals = AgentPlatform.m_Platform.f_Find("=");
				if (iEquals >= 0)
				{
					AgentPlatform.m_LocalPath = AgentPlatform.m_Platform.f_Extract(iEquals + 1);
					AgentPlatform.m_Platform = AgentPlatform.m_Platform.f_Left(iEquals);
				}

				if (bPackageIsAgent && AgentPlatform.m_LocalPath.f_IsEmpty() && AgentPlatform.m_Platform == DetectedPlatform)
					continue;

				if (AgentPlatform.m_LocalPath.f_IsEmpty() && bFromFile)
				{
					co_return DMibErrorInstance
						(
							"Agent platform '{}' needs a local file or directory when using --from-file. "
							"Specify it as <Platform>=<Path>, for example {}=/opt/Deploy/AppManager_Linux_arm64."_f
							<< AgentPlatform.m_Platform
							<< AgentPlatform.m_Platform
						)
					;
				}

				AgentPlatforms.f_Insert(fg_Move(AgentPlatform));
			}
		}

		// The declared platform names the application that the environments of that platform run, so a local package with metadata of another
		// platform is rejected before anything is installed
		for (auto &AgentPlatform : AgentPlatforms)
		{
			if (AgentPlatform.m_LocalPath.f_IsEmpty())
				continue;

			auto PackageInfo = co_await fp_ReadPackageVersionInfo
				(
					CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(AgentPlatform.m_LocalPath, mp_State.m_RootDirectory))
					, [_pCommandLine](CStr const &_Info)
					{
						*_pCommandLine += _Info + DMibNewLine;
					}
				)
			;

			// The installed platform and the update source come from the metadata, and without them the application is no agent distribution
			if (!PackageInfo)
				co_return DMibErrorInstance("The package '{}' for agent platform '{}' has no readable version info"_f << AgentPlatform.m_LocalPath << AgentPlatform.m_Platform);

			// The add reads the package again, which is a file of the operator on the host, which is trusted. A package that is replaced after this
			// check installs what the operator put there, and with a version of another platform the application is no agent distribution, see
			// CApplication::f_AgentDistributionPlatform
			if (PackageInfo->m_VersionID.m_Platform != AgentPlatform.m_Platform)
			{
				co_return DMibErrorInstance
					(
						"The package '{}' for agent platform '{}' is for platform '{}'"_f << AgentPlatform.m_LocalPath << AgentPlatform.m_Platform << PackageInfo->m_VersionID.m_Platform
					)
				;
			}
		}

		CStr MainName = bPackageIsAgent ? CStr("SelfUpdate.{}"_f << DetectedPlatform) : _Params["Name"].f_String();

		// Installed applications are kept, so that agents can be added to an installation with self update enabled and an interrupted run can be
		// repeated. Without agent platforms an installed self update application fails the command as before. An agent application is kept only
		// when it is the agent distribution of its platform, since environments of the platform run what the application of that name installs,
		// and the host application only when it is the self update source
		auto fIsInstalled = [&](CStr const &_Name, CStr const &_AgentPlatform, CStr &o_Error)
			{
				auto *pApplication = mp_Applications.f_FindEqual(_Name);
				if (!pApplication)
					return false;

				if (!_AgentPlatform.f_IsEmpty() && (*pApplication)->f_AgentDistributionPlatform() != _AgentPlatform)
				{
					o_Error = "Application '{}' is installed, but isn't the agent distribution for platform '{}'"_f << _Name << _AgentPlatform;
					return true;
				}

				if (_AgentPlatform.f_IsEmpty() && !(*pApplication)->m_Settings.m_bSelfUpdateSource)
				{
					o_Error = "Application '{}' is installed, but isn't the self update source"_f << _Name;
					return true;
				}

				*_pCommandLine += "Application '{}' is already installed\n"_f << _Name;
				return true;
			}
		;

		// The command reports which of the applications are installed, which only callers with access to them may learn, as from f_GetInstalled
		if (!AgentPlatforms.f_IsEmpty())
		{
			auto Auditor = f_Auditor({}, CallingHostInfo);

			NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
			auto fAddInstalledPermission = [&](CStr const &_Name)
				{
					if (!mp_Applications.f_FindEqual(_Name))
						return;

					Permissions[_Name] =
						{
							CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)
						}
					;
				}
			;

			fAddInstalledPermission(MainName);
			for (auto &AgentPlatform : AgentPlatforms)
				fAddInstalledPermission("SelfUpdate.{}"_f << AgentPlatform.m_Platform);

			if (!Permissions.f_IsEmpty())
			{
				NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
					(
						mp_Permissions.f_HasPermissions("Enable self update in AppManager", Permissions, CallingHostInfo) % "Permission denied enabling self update" % Auditor
					)
				;

				for (auto &bHasPermission : HasPermissions)
				{
					if (!bHasPermission)
					{
						auto &Key = HasPermissions.fs_GetKey(bHasPermission);
						co_return Auditor.f_AccessDenied(CStr("(Enable self update, App/{})"_f << Key), Permissions[Key]);
					}
				}
			}
		}

		// Adding the host installation can self update, which restarts this AppManager, so the agents are added first
		for (auto &AgentPlatform : AgentPlatforms)
		{
			CStr InstalledError;
			if (fIsInstalled(CStr("SelfUpdate.{}"_f << AgentPlatform.m_Platform), AgentPlatform.m_Platform, InstalledError))
			{
				if (!InstalledError.f_IsEmpty())
					co_return DMibErrorInstance(InstalledError);

				continue;
			}

			CEJsonSorted PlatformParams = _Params;
			PlatformParams["Name"] = "SelfUpdate.{}"_f << AgentPlatform.m_Platform;
			PlatformParams["VersionManagerPlatform"] = AgentPlatform.m_Platform;
			PlatformParams["SelfUpdateSource"] = false;
			PlatformParams["SettingsFromVersionInfo"] = false;

			if (!AgentPlatform.m_LocalPath.f_IsEmpty())
			{
				PlatformParams["FromFile"] = true;
				PlatformParams["Package"] = AgentPlatform.m_LocalPath;
			}

			uint32 PlatformStatus = co_await fp_CommandLine_AddApplication(fg_Move(PlatformParams), _pCommandLine);
			if (PlatformStatus != 0)
				co_return PlatformStatus;
		}

		if (!AgentPlatforms.f_IsEmpty())
		{
			CStr InstalledError;
			if (fIsInstalled(MainName, bPackageIsAgent ? DetectedPlatform : CStr(), InstalledError))
			{
				if (!InstalledError.f_IsEmpty())
					co_return DMibErrorInstance(InstalledError);

				co_return 0;
			}
		}

		uint32 Status;
		if (bPackageIsAgent)
		{
			CEJsonSorted AgentParams = _Params;
			AgentParams["Name"] = "SelfUpdate.{}"_f << DetectedPlatform;
			AgentParams["VersionManagerPlatform"] = DetectedPlatform;
			AgentParams["SelfUpdateSource"] = false;
			AgentParams["SettingsFromVersionInfo"] = false;

			Status = co_await fp_CommandLine_AddApplication(fg_Move(AgentParams), _pCommandLine);
		}
		else
			Status = co_await fp_CommandLine_AddApplication(_Params, _pCommandLine);

		co_return Status;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_AddApplication(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
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

		bool bNullPackage = _Params["Package"].f_Type() == EJsonType_Null;

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

		auto Result = co_await fp_AddApplication
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
				, CallingHostInfo
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	auto CAppManagerActor::fp_AddApplication
		(
			NStr::CStr _Name
			, CApplicationSettings _Settings
			, EApplicationSetting _ChangedSettings
			, bool _bForceOverwrite
			, bool _bForceInstall
			, bool _bSettingsFromVersionInfo
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CStr _FromLocalFile
			, TCOptional<CVersionManager::CVersionIDAndPlatform> _Version
			, CCallingHostInfo _CallingHostInfo
		)
		-> TCFuture<void>
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationAdd"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty())
			Permissions["Version"] = {{"AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", _Settings.m_VersionManagerApplication)}};

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Add application to AppManager", Permissions, _CallingHostInfo) % "Permission denied adding application" % Auditor
			)
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
					auto fFindVersion = [&]
						{
							return CAppManagerActor::fp_FindVersion
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
						}
					;

					// The subscription of a version manager sends its newest versions across the platforms, which can leave out the newest version
					// of the platform of an agent distribution, so its add lists the versions of the platform from every version manager first
					bool bAgentDistribution = !pApplication->m_Settings.m_bSelfUpdateSource && pApplication->m_Name.f_StartsWith("SelfUpdate.");
					if (bAgentDistribution)
						co_await fp_VersionManagerListPlatformVersions(VersionManagerApplication, Platform);

					VersionID = fFindVersion();
					if (!VersionID.f_IsValid() && !bAgentDistribution)
					{
						co_await fp_VersionManagerListPlatformVersions(VersionManagerApplication, Platform);
						VersionID = fFindVersion();
					}

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

		if (!pApplication->m_Settings.m_LaunchEnvironment.f_IsEmpty() && !fp_EnvironmentAcceptsApplications(pApplication->m_Settings.m_LaunchEnvironment))
			co_return Auditor.f_Exception(fg_Format("Launch environment '{}' does not exist. Add it with --environment-add.", pApplication->m_Settings.m_LaunchEnvironment));

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

		auto Cleanup = g_OnScopeExitActor / [pApplication]
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

		co_await (fp_ChangeEncryption(pApplication, EEncryptOperation_Setup, _bForceOverwrite) % Auditor);

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

					// Reapply settings specified coming into this function
					NewSettings.f_ApplySettings(_ChangedSettings, pApplication->m_Settings);

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
					fp_VersionManagerResubscribeAll().f_DiscardResult();
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
			auto PackageInfo = co_await (fp_ReadPackageVersionInfo(_FromLocalFile, _fOnInfo) % "Failed to read version info from package" % Auditor);

			if (PackageInfo)
			{
				bool bTakeUpdateSource = false;
				try
				{
					fApplyVersion(PackageInfo->m_VersionID, PackageInfo->m_VersionInfo);

					// The update source is a setting of the package, which an add without settings from the version info leaves unset, so that
					// the version manager doesn't replace the package. An agent distribution, whose settings describe the guest, takes its update
					// source from the package also without them
					bTakeUpdateSource = _bSettingsFromVersionInfo || !pApplication->f_AgentDistributionPlatform().f_IsEmpty();
				}
				catch (CException const &_Exception)
				{
					_fOnInfo("Failed to apply version info from package: {}"_f << _Exception);
				}

				// The caller needs access to the version manager application that the package names, like to one it names itself
				if (bTakeUpdateSource && PackageInfo->m_Application != _Settings.m_VersionManagerApplication && !PackageInfo->m_Application.f_IsEmpty())
				{
					NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> VersionPermissions;
					VersionPermissions["Version"] = {{"AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", PackageInfo->m_Application)}};

					NContainer::TCMap<NStr::CStr, bool> HasVersionPermissions = co_await
						(
							mp_Permissions.f_HasPermissions("Add application to AppManager", VersionPermissions, _CallingHostInfo) % "Permission denied adding application" % Auditor
						)
					;

					if (!HasVersionPermissions["Version"])
						co_return Auditor.f_AccessDenied("(Application add, version application of the package)", VersionPermissions["Version"]);
				}

				if (bTakeUpdateSource)
					pApplication->m_Settings.m_VersionManagerApplication = PackageInfo->m_Application;
			}

			SourcePath = _FromLocalFile;
		}
		else
		{
			CStr DownloadDirectoryRoot = Directory / "TempVersionDownload";
			CStr DownloadDirectory = DownloadDirectoryRoot / fg_FastRandomID();
			_fOnInfo(fg_Format("Downloading version '{}' from version managers", VersionID));
			auto VersionInfo = co_await (fp_DownloadApplication(VersionManagerApplication, VersionID, DownloadDirectory) % "Failed to download application from version manager" % Auditor);
			{
				auto CaptureScope = co_await (g_CaptureExceptions % Auditor);
				fApplyVersion(VersionID, VersionInfo);
			}

			*pDeletePath = DownloadDirectoryRoot;
			SourcePath = DownloadDirectory;
		}

		// Package settings can change the launch environment after the requested one was checked
		if (!pApplication->m_Settings.m_LaunchEnvironment.f_IsEmpty() && !fp_EnvironmentAcceptsApplications(pApplication->m_Settings.m_LaunchEnvironment))
			co_return Auditor.f_Exception(fg_Format("Launch environment '{}' does not exist. Add it with --environment-add.", pApplication->m_Settings.m_LaunchEnvironment));

		if (!pApplication->m_Settings.m_LaunchEnvironment.f_IsEmpty())
			co_await fp_CheckEnvironmentAccess(pApplication->m_Settings.m_LaunchEnvironment, _CallingHostInfo);

#ifdef DPlatformFamily_Windows
		if (!pApplication->m_Settings.m_RunAsUser.f_IsEmpty() && pApplication->m_Settings.m_RunAsUserPassword.f_IsEmpty())
			pApplication->m_Settings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
#endif
		_fOnInfo("Unpacking application");

		TCVector<CStr> Files;
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			auto [UnpackedFiles, LowPortExecutablesToGrant] = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [=, pUniqueUserGroup = mp_pUniqueUserGroup, RootDirectory = mp_State.m_RootDirectory]
					() mutable -> TCTuple<TCVector<CStr>, TCMap<CStr, CStr>>
					{
						auto &Settings = pApplication->m_Settings;
						fsp_CreateApplicationUserGroup(Settings, _fOnInfo, Directory / ".home", pUniqueUserGroup);

						TCVector<CStr> Files;

						if (!bNullApplication)
						{
							if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
							{
								auto Files = CFile::fs_FindFiles(SourcePath + "/*");
								if (Files.f_GetLen() == 1 && (Files[0].f_EndsWith(".tar.gz") || Files[0].f_EndsWith(".tar.zst") || Files[0].f_EndsWith(".tar")))
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

						auto LowPortExecutablesToGrant = fsp_UpdateApplicationFilePermissions(Directory, pApplication, pApplication->m_Files, pUniqueUserGroup, _fOnInfo);

						return {fg_Move(Files), fg_Move(LowPortExecutablesToGrant)};
					}
					% "Failed to unpack application" % Auditor
				)
			;

			Files = fg_Move(UnpackedFiles);
			pApplication->m_LowPortExecutablesToGrant = fg_Move(LowPortExecutablesToGrant);
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
		Cleanup->f_Clear();

		auto InProgressScope = pApplication->f_SetInProgress("Add Application");
		auto DestroyInProgress = co_await fg_AsyncDestroy(fg_Move(InProgressScope));

		fp_OnApplicationAdded(pApplication);

		pApplication->m_LastInstalledVersionFinished = pApplication->m_LastInstalledVersion;
		pApplication->m_LastInstalledVersionInfoFinished = pApplication->m_LastInstalledVersionInfo;

		co_await (fp_UpdateApplicationJson(pApplication) % "Failed to save state" % Auditor);

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

	// _Package is a package, or a directory that only holds a .tar, .tar.gz or .tar.zst archive, which is read as that archive.
	// Returns no value when the package metadata is missing, fails to extract or doesn't parse; extraction and parse failures are reported through
	// _fOnInfo. Errors reading a directory package fail the future
	auto CAppManagerActor::fp_ReadPackageVersionInfo(CStr _Package, TCFunction<void (CStr const &_Info)> _fOnInfo) -> TCFuture<TCOptional<CPackageVersionInfo>>
	{
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			_Package = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [_Package]() -> CStr
					{
						if (!CFile::fs_FileExists(_Package, EFileAttrib_Directory))
							return _Package;

						auto Files = CFile::fs_FindFiles(_Package / "*");
						if (Files.f_GetLen() == 1 && (Files[0].f_EndsWith(".tar.gz") || Files[0].f_EndsWith(".tar.zst") || Files[0].f_EndsWith(".tar")))
							return Files[0];

						return _Package;
					}
				)
			;
		}

		CStr VersionInfoContents;
		CTime VersionTime;

		if (_Package.f_EndsWith(".tar.gz") || _Package.f_EndsWith(".tar.zst") || _Package.f_EndsWith(".tar"))
		{
			TCActor<CProcessLaunchActor> LaunchActor(fg_Construct());
			auto AutoDestroyLaunch = co_await fg_AsyncDestroy(LaunchActor);

			CProcessLaunchActor::CSimpleLaunch Launch
				{
					mp_State.m_RootDirectory / "bin/bsdtar"
					,
					{
						"-xqOf"
						, _Package
						, "*VersionInfo.json"
					}
					, CFile::fs_GetPath(_Package)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			TCActor<CProcessLaunchActor> LaunchListActor(fg_Construct());
			auto AutoDestroyLaunchList = co_await fg_AsyncDestroy(LaunchListActor);

			CProcessLaunchActor::CSimpleLaunch LaunchList
				{
					mp_State.m_RootDirectory / "bin/bsdtar"
					,
					{
						"-tvvf"
						, _Package
					}
					, CFile::fs_GetPath(_Package)
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				}
			;

			auto [LaunchResult, LaunchListResult] = co_await
				(
					LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch)) + LaunchListActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(LaunchList))
				)
				.f_Wrap()
			;

			if (!LaunchResult)
			{
				_fOnInfo("Failed to extract version info from package: {}"_f << LaunchResult.f_GetExceptionStr());
				co_return {};
			}

			if (!LaunchListResult)
				_fOnInfo("Failed to extract version time from package: {}"_f << LaunchListResult.f_GetExceptionStr());
			else
			{
				auto ListStr = (*LaunchListResult).f_GetStdOut();
				CTime Newest;
				for (auto &Line : ListStr.f_SplitLine<true>())
				{
					ch8 const *pParse = Line.f_GetStr();

					auto fParseField = [&]() -> CStr
						{
							ch8 const *pStart = pParse;
							fg_ParseNonWhiteSpaceAndSeparators(pParse, "");
							CStr Field(pStart, pParse - pStart);
							fg_ParseWhiteSpace(pParse);
							return Field;
						}
					;

					// The columns are read as CVersionManagerHelper reads them for an archive it uploads, so that a package has the same time
					// on both sides, also when a user or group name with spaces shifts them
					fParseField(); // Permissions
					fParseField(); // ?
					fParseField(); // User
					fParseField(); // Group
					fParseField(); // Size or device number

					CStr UnixSeconds = fParseField();
					CStr NanoSeconds = fParseField();

					auto FileTime = CTimeConvert::fs_FromUnixSeconds(UnixSeconds.f_ToInt(int64(0)));
					FileTime.f_SetFraction(fp64(NanoSeconds.f_ToInt(int32(0))) / fp64(1'000'000'000.0));

					if (!Newest.f_IsValid() || FileTime > Newest)
						Newest = FileTime;
				}

				VersionTime = CTimeConvert::fs_FromUnixMilliseconds(CTimeConvert(Newest).f_UnixMilliseconds());
			}

			VersionInfoContents = LaunchResult->f_GetStdOut();
		}
		else
		{
			auto BlockingActorCheckout = fg_BlockingActor();
			auto [Contents, Time] = co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [_Package]
						{
							if (!CFile::fs_FileExists(_Package, EFileAttrib_Directory))
								return fg_Tuple(CStr{}, CTime{});

							CStr VersionInfoFile;

							if (CFile::fs_FileExists(_Package / "VersionInfo.json", EFileAttrib_File))
								VersionInfoFile = _Package / "VersionInfo.json";
							else
							{
								auto Files = CFile::fs_FindFiles(_Package / "*VersionInfo.json", EFileAttrib_File, false);

								if (Files.f_GetLen() != 1)
									return fg_Tuple(CStr{}, CTime{});

								VersionInfoFile = Files[0];
							}

							// The newest file of the package in milliseconds, as CVersionManagerHelper::f_CreatePackage chooses the version time of a
							// directory it uploads
							CFile::CFindFilesOptions FindOptions{_Package + "/*", true};
							FindOptions.m_AttribMask = EFileAttrib_File;
							CTime Newest;
							for (auto &FoundFile : CFile::fs_FindFiles(FindOptions))
							{
								// A link can name a target that only exists where the package is installed, like in a guest, and has the time of
								// the link itself then, which the unpack copies as a link
								CTime FoundTime;
								if (FoundFile.m_Attribs & EFileAttrib_Link)
								{
									try
									{
										FoundTime = CFile::fs_GetWriteTime(FoundFile.m_Path);
									}
									catch (CExceptionFile const &)
									{
										FoundTime = CFile::fs_GetWriteTimeOnLink(FoundFile.m_Path);
									}
								}
								else
									FoundTime = CFile::fs_GetWriteTime(FoundFile.m_Path);

								if (!Newest.f_IsValid() || FoundTime > Newest)
									Newest = FoundTime;
							}

							CTime VersionTime = CTimeConvert::fs_FromUnixMilliseconds(CTimeConvert(Newest).f_UnixMilliseconds());
							return fg_Tuple(CFile::fs_ReadStringFromFile(VersionInfoFile, true), VersionTime);
						}
					)
					% "Failed to read version info from package"
				)
			;

			VersionInfoContents = fg_Move(Contents);
			VersionTime = Time;
		}

		if (VersionInfoContents.f_IsEmpty())
			co_return {};

		CPackageVersionInfo PackageInfo;

		try
		{
			CEJsonSorted VersionInfoJson = CEJsonSorted::fs_FromString(VersionInfoContents);

			CStr Application;
			CStr Version;
			CStr Configuration;
			CStr Platform;
			CEJsonSorted ExtraInfo;

			if (auto *pValue = VersionInfoJson.f_GetMember("Application", EJsonType_String))
				Application = pValue->f_String();
			if (auto *pValue = VersionInfoJson.f_GetMember("Version", EJsonType_String))
				Version = pValue->f_String();
			if (auto *pValue = VersionInfoJson.f_GetMember("Configuration", EJsonType_String))
				Configuration = pValue->f_String();
			if (auto *pValue = VersionInfoJson.f_GetMember("Platform", EJsonType_String))
				Platform = pValue->f_String();
			if (auto *pValue = VersionInfoJson.f_GetMember("ExtraInfo", EJsonType_Object))
				ExtraInfo = pValue->f_Object();

			if (Application.f_IsEmpty())
				DMibError("Application must be specified");
			if (Version.f_IsEmpty())
				DMibError("Version must be specified");

			if (!CVersionManager::fs_IsValidApplicationName(Application))
				DMibError("Application format is invalid");

			{
				CStr Error;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, Error, &PackageInfo.m_VersionID.m_VersionID))
					DMibError(fg_Format("Version identifier format is invalid: {}", Error));
			}

			if (!CVersionManager::fs_IsValidPlatform(Platform))
				DMibError("Invalid version platform format");

			PackageInfo.m_VersionID.m_Platform = Platform;
			PackageInfo.m_VersionInfo.m_Configuration = Configuration;
			PackageInfo.m_VersionInfo.m_ExtraInfo = fg_Move(ExtraInfo);
			PackageInfo.m_VersionInfo.m_Time = VersionTime;
			PackageInfo.m_Application = fg_Move(Application);
		}
		catch (CException const &_Exception)
		{
			_fOnInfo("Failed to parse version info from VersionInfo.json in package: {}"_f << _Exception);
			co_return {};
		}

		co_return fg_Move(PackageInfo);
	}
}
