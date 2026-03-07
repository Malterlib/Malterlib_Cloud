// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Concurrency/AsyncDestroy>

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_ChangeSettings
		(
			NStr::CStr _Name
			, CApplicationChangeSettings _ChangeSettings
			, CApplicationSettings _Settings
		)
	{

		CAppManagerActor::CApplicationSettings ApplicationSettings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		ApplicationSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);

		co_return co_await m_pThis->fp_ChangeApplicationSettings
			(
				_Name
				, ApplicationSettings
				, ChangedSettings
				, _ChangeSettings.m_bUpdateFromVersionInfo
				, _ChangeSettings.m_bForce
				, [](CStr const &_Info)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Change Settings: {}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
		;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_ChangeApplicationSettings(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();
		fp_ReportInProgress(_pCommandLine, Name);

		CApplicationSettings Settings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;

		bool bUpdateFromVersionInfo = _Params["UpdateFromVersionInfo"].f_Boolean();
		bool bForce = _Params["Force"].f_Boolean();

		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error, false))
				co_return DMibErrorInstance(Error);
		}

		auto Result = co_await fp_ChangeApplicationSettings
			(
				Name
				, Settings
				, ChangedSettings
				, bUpdateFromVersionInfo
				, bForce
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
				}
				, fg_GetCallingHostInfo()
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}

	TCFuture<void> CAppManagerActor::fp_UpdateAppManagerApplicationVersion(TCSharedPointer<CApplication> _pApplication, uint32 _OldVersion)
	{
		auto fOnInfo = [Name = _pApplication->m_Name](CStr const &_Info)
			{
				DMibLogCategory(Name);
				DMibLog(Info, "App manager version upgrade: {}", _Info);
			}
		;

		fOnInfo("Upgrading from {nfh} to {nfh}"_f << _OldVersion << mcp_CurrentAppMangerVersion);

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				g_Dispatch(BlockingActorCheckout) / [=, Directory = _pApplication->f_GetDirectory(), pUniqueUserGroup = mp_pUniqueUserGroup]() mutable
				{
					if (_OldVersion < 0x101)
					{
						// Naming of user names changed

						fsp_CreateApplicationUserGroup(_pApplication->m_Settings, fOnInfo, Directory / ".home", pUniqueUserGroup);

						CStr NewOwner = pUniqueUserGroup->f_GetUser(_pApplication->m_Settings.m_RunAsUser);
						CStr NewGroup = pUniqueUserGroup->f_GetGroup(_pApplication->m_Settings.m_RunAsGroup);

						CStr OldOwner = CFile::fs_GetOwner(Directory);
						CStr OldGroup = CFile::fs_GetGroup(Directory);

						bool bChangedOwner = !NewOwner.f_IsEmpty() && NewOwner != OldOwner;
						bool bChangedGroup = !NewGroup.f_IsEmpty() && NewGroup != OldGroup;

						if (bChangedOwner || bChangedGroup)
						{
							for (auto &File : CFile::fs_FindFiles(Directory / "*", EFileAttrib_File | EFileAttrib_Directory, true, false))
							{
								if (bChangedOwner)
								{
									CStr CurrentOwner = CFile::fs_GetOwner(File);
									if (CurrentOwner == OldOwner)
										CFile::fs_SetOwner(File, NewOwner);
								}

								if (bChangedGroup)
								{
									CStr CurrentGroup = CFile::fs_GetGroup(File);
									if (CurrentGroup == OldGroup)
										CFile::fs_SetGroup(File, NewGroup);
								}
							}
						}

						fsp_UpdateApplicationFilePermissions(Directory, _pApplication, _pApplication->m_Files, pUniqueUserGroup, fOnInfo);
					}
				}
			)
		;

		if (_OldVersion < 0x101)
		{
			if (_pApplication->m_LastTriedInstalledVersionInfo.m_ExtraInfo.f_GetMemberValue("DistributedApp", false).f_Boolean())
				_pApplication->m_Settings.m_bDistributedApp = true;
		}

		fOnInfo("Upgrading from {nfh} to {nfh} finished"_f << _OldVersion << mcp_CurrentAppMangerVersion);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_ChangeApplicationSettings
		(
			NStr::CStr _Name
			, CApplicationSettings _Settings
			, EApplicationSetting _ChangedSettings
			, bool _bUpdateFromVersionInfo
			, bool _bForce
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
		)
	{
		auto Auditor = f_Auditor({}, _CallingHostInfo);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationChangeSettings"}};
		Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty() && (_ChangedSettings & EApplicationSetting_VersionManagerApplication))
		{
			CStr const &App = _Settings.m_VersionManagerApplication;
			Permissions["Version"] = {CPermissionQuery{"AppManager/VersionAppAll", "AppManager/VersionApp/{}"_f << App}.f_Description("Access application {} in AppManager"_f << App)};
		}

		NContainer::TCMap<NStr::CStr, bool> HasPermissions = co_await
			(
				mp_Permissions.f_HasPermissions("Change applications settings in AppManager", Permissions, _CallingHostInfo) % "Permission denied changing application settings" % Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(Application change settings, command)", Permissions["Command"]);

		if (!HasPermissions["App"])
			co_return Auditor.f_AccessDenied("(Application change settings, app name)", Permissions["App"]);

		if (auto *pVersion = HasPermissions.f_FindEqual("Version"))
		{
			if (!*pVersion)
				co_return Auditor.f_AccessDenied("(Application change settings, version application)", Permissions["Version"]);
		}

		auto *pFindApplication = mp_Applications.f_FindEqual(_Name);
		if (!pFindApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		auto pApplication = *pFindApplication;

		auto &Application = *pApplication;

		CApplicationSettings Settings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;

		if (_bUpdateFromVersionInfo)
		{
			if (!Application.m_LastInstalledVersion.f_IsValid())
				co_return Auditor.f_Exception("Found no install from last version to get settings from");
			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Failed to get settings from version info" % Auditor);
				Settings.f_FromVersionInfo(Application.m_LastInstalledVersionInfo, ChangedSettings);
			}
		}

		Settings.f_ApplySettings(_ChangedSettings, _Settings);
		ChangedSettings |= _ChangedSettings;

		auto NewSettings = Application.m_Settings;
		NewSettings.f_ApplySettings(ChangedSettings, Settings);

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				co_return Auditor.f_Exception(Error);
		}

		ChangedSettings = Application.m_Settings.f_ChangedSettings(NewSettings);

#ifdef DPlatformFamily_Windows
		if (!NewSettings.m_RunAsUser.f_IsEmpty() && ((ChangedSettings & EApplicationSetting_RunAsUser) || NewSettings.m_RunAsUserPassword.f_IsEmpty()))
		{
			NewSettings.m_RunAsUserPassword = fg_HighEntropyRandomID("23456789ABCDEFGHJKLMNPQRSTWXYZabcdefghijkmnopqrstuvwxyz&=*!@~^") + "2Dg&";
			ChangedSettings |= EApplicationSetting_RunAsUserPassword;
		}
#endif

		if (ChangedSettings & EApplicationSetting_EncryptionStorage)
			co_return Auditor.f_Exception("Changing encryption storage is not supported");
		if (ChangedSettings & EApplicationSetting_EncryptionFileSystem)
			co_return Auditor.f_Exception("Changing encryption file system is not supported");
		if (ChangedSettings & EApplicationSetting_ParentApplication)
			co_return Auditor.f_Exception("Changing parent application is not supported");
		if (ChangedSettings == EApplicationSetting_None && !_bForce)
		{
			_fOnInfo("No settings were changed. To update file permissions run with --force");
			co_return {};
		}

		auto InProgressScope = co_await (fp_SetInProgressWithWait(pApplication, "ChangeApplicationSettings") % Auditor);
		auto DestroyInProgress = co_await fg_AsyncDestroy(fg_Move(InProgressScope));

		if (!(ChangedSettings & EApplicationSetting_NeedUpdateSettings) && !_bForce)
		{
			Application.m_Settings = NewSettings;
			if (ChangedSettings & (EApplicationSetting_VersionManagerApplication | EApplicationSetting_UpdateGroup | EApplicationSetting_AutoUpdate))
				fp_OnAppUpdateInfoChange(pApplication);

			fp_SendAppChange_AddedOrChanged(Application);

			_fOnInfo("Saving application state");
			co_await (fp_UpdateApplicationJson(pApplication) % "Failed to save application state" % Auditor);

			if (ChangedSettings & EApplicationSetting_BackupEnabled)
			{
				if (Application.m_Settings.m_bBackupEnabled)
					fp_ApplicationStartBackup(pApplication);
				else if (Application.m_BackupClient)
					co_await (fg_Move(Application.m_BackupClient).f_Destroy() % "Failed to stop backup client" % Auditor);
			}

			_fOnInfo("Application settings were successfully changed");
			Auditor.f_Info("Updated application settings (No restart required)");

			co_return {};
		}

		co_await (fp_ChangeEncryption(pApplication, EEncryptOperation_Open, false) % "Failed to open encryption" % Auditor);

		if (pApplication->m_bDeleted)
			co_return Auditor.f_Exception("Application has been deleted, aborting");

		_fOnInfo("Stopping old application");

		TCAsyncResult<uint32> ExitStatus = co_await pApplication->f_Stop(EStopFlag_None).f_Wrap();

		CStr Error = fp_GetApplicationStopErrors(ExitStatus, pApplication->m_Name);

		if (!Error.f_IsEmpty())
		{
			_fOnInfo(Error);
			Auditor.f_Warning(Error);
		}

		if (!ExitStatus)
			co_return Auditor.f_Exception("Failed to exit old application, aborting update");

		if (pApplication->m_bDeleted)
			co_return Auditor.f_Exception("Application has been deleted, aborting");

		pApplication->m_Settings = NewSettings;
		if (ChangedSettings & (EApplicationSetting_VersionManagerApplication | EApplicationSetting_UpdateGroup | EApplicationSetting_AutoUpdate))
			fp_OnAppUpdateInfoChange(pApplication);

		fp_SendAppChange_AddedOrChanged(*pApplication);

		_fOnInfo("Saving application state and update application files");

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			auto [Result, UpdateJsonResults] = co_await
				(
					(
						g_Dispatch(BlockingActorCheckout)
						/ [=, Directory = pApplication->f_GetDirectory(), pUniqueUserGroup = mp_pUniqueUserGroup]()
						{
							fsp_CreateApplicationUserGroup(NewSettings, _fOnInfo, Directory / ".home", pUniqueUserGroup);
							fsp_UpdateApplicationFilePermissions(Directory, pApplication, pApplication->m_Files, pUniqueUserGroup, _fOnInfo);
						}
					)
					+ fp_UpdateApplicationJson(pApplication)
				)
				.f_Wrap()
			;

			if (!Result && !UpdateJsonResults)
				co_return Auditor.f_Exception("Failed to update application files and save application state: {} {}"_f << Result.f_GetExceptionStr() << UpdateJsonResults.f_GetExceptionStr());
			else if (!Result)
				co_return Auditor.f_Exception("Failed to update application files: {}"_f << Result.f_GetExceptionStr());
			else if (!UpdateJsonResults)
				co_return Auditor.f_Exception("Failed to save application state: {}"_f << UpdateJsonResults.f_GetExceptionStr());
			else
				_fOnInfo("Application state successfully stored, so any changes will persist");
		}

		if (pApplication->m_bDeleted)
			co_return Auditor.f_Exception("Application has been deleted, aborting");

		pApplication->m_bPreventLaunch_DelayAfterFailure = false;

		CStr DependenciesMessage;
		CAppManagerInterface::EStatusSeverity Severity;
		bool bNeedsEncryption = false;
		if (!pApplication->f_DependenciesSatisfied(DependenciesMessage, Severity, bNeedsEncryption))
		{
			_fOnInfo(fg_Format("Application settings were successfully changed. Launch skipped because of missing dependencies: {}", DependenciesMessage));
			Auditor.f_Info("Updated application settings");

			fp_SetAppLaunchStatus(pApplication, DependenciesMessage, Severity);

			co_return {};
		}

		_fOnInfo("Launching applicaion with changed settings");

		CAppLaunchResult LaunchResult = co_await (fp_LaunchApp(pApplication, false) % "Failed to launch app. Will retry periodically." % Auditor);

		_fOnInfo("Application settings were successfully changed");
		if (LaunchResult.m_StartupError)
			_fOnInfo("Application startup failed: {}"_f << LaunchResult.m_StartupError);

		Auditor.f_Info("Updated application settings");

		co_await fg_Move(DestroyInProgress);
		co_await fp_SyncNotifications(_Name);

		co_return {};
	}
}
