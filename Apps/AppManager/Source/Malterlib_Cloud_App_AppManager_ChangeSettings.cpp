// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCContinuation<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_ChangeSettings
		(
			NStr::CStr const &_Name
			, CApplicationChangeSettings const &_ChangeSettings
			, CApplicationSettings const &_Settings
		)
	{
		CAppManagerActor::CApplicationSettings ApplicationSettings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		ApplicationSettings.f_FromInterfaceSettings(_Settings, ChangedSettings);
		return m_pThis->fp_ChangeApplicationSettings
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
			)
		;
	}

	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		
		CApplicationSettings Settings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		
		bool bUpdateFromVersionInfo = _Params["UpdateFromVersionInfo"].f_Boolean();
		bool bForce = _Params["Force"].f_Boolean();

		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error, false))
				return DMibErrorInstance(Error);
		}

		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		TCSharedPointer<CDistributedAppCommandLineResults> pResult = fg_Construct();
		
		fp_ChangeApplicationSettings
			(
				Name
				, Settings
				, ChangedSettings
				, bUpdateFromVersionInfo
				, bForce
				, [pResult](CStr const &_Info)
				{
					pResult->f_AddStdOut(_Info + DMibNewLine);
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", _Info);
				}
			)
			> [=](TCAsyncResult<void> &&_Result)
			{
				pResult->f_AddAsyncResult(_Result);
				Continuation.f_SetResult(fg_Move(*pResult));
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_ChangeApplicationSettings
		(
			NStr::CStr const &_Name
			, CApplicationSettings const &_Settings
			, EApplicationSetting _ChangedSettings
			, bool _bUpdateFromVersionInfo
			, bool _bForce
			, TCFunction<void (CStr const &_Info)> &&_fOnInfo
		)
	{
		auto Auditor = f_Auditor();
		auto CallingHostID = fg_GetCallingHostID();

		if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationChangeSettings"))
			return Auditor.f_AccessDenied("(Application change settings, command)");

		if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)))
			return Auditor.f_AccessDenied("(Application change settings, app name)");
		
		if (!_Settings.m_VersionManagerApplication.f_IsEmpty() && (_ChangedSettings & EApplicationSetting_VersionManagerApplication))
		{
			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/VersionAppAll", fg_Format("AppManager/VersionApp/{}", _Settings.m_VersionManagerApplication)))
				return Auditor.f_AccessDenied("(Application change settings, version application)");
		}
		
		auto *pApplication = mp_Applications.f_FindEqual(_Name);
		if (!pApplication)
			return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));
		auto &Application = **pApplication;

		CApplicationSettings Settings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		
		if (_bUpdateFromVersionInfo)
		{
			if (!Application.m_LastInstalledVersion.f_IsValid())
				return Auditor.f_Exception("Found no install from last version to get settings from");
			Settings.f_FromVersionInfo(Application.m_LastInstalledVersionInfo, ChangedSettings);
		}
		
		Settings.f_ApplySettings(_ChangedSettings, _Settings);
		ChangedSettings |= _ChangedSettings;
		
		auto NewSettings = Application.m_Settings;
		NewSettings.f_ApplySettings(ChangedSettings, Settings);	

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				return Auditor.f_Exception(Error);
		}
		
 		ChangedSettings = Application.m_Settings.f_ChangedSettings(NewSettings);

		if (ChangedSettings & EApplicationSetting_EncryptionStorage)
			return Auditor.f_Exception("Changing encryption storage is not supported");
		if (ChangedSettings & EApplicationSetting_EncryptionFileSystem)
			return Auditor.f_Exception("Changing encryption file system is not supported");
		if (ChangedSettings & EApplicationSetting_ParentApplication)
			return Auditor.f_Exception("Changing parent application is not supported");
		if (ChangedSettings == EApplicationSetting_None && !_bForce)
		{
			_fOnInfo("No settings were changed. To update file permissions run with --force");
			return fg_Explicit();
		}
		
		if (Application.f_IsInProgress())
			return Auditor.f_Exception("Operation already in progress for application");

		auto InProgressScope = Application.f_SetInProgress();

		TCContinuation<void> Continuation;
		
		if (!(ChangedSettings & EApplicationSetting_NeedUpdateSettings) && !_bForce)
		{
			Application.m_Settings = NewSettings;
			if (ChangedSettings & (EApplicationSetting_VersionManagerApplication | EApplicationSetting_UpdateGroup))
				fp_RemoteAppInfoChanged(*pApplication);
			_fOnInfo("Saving application state");
			fp_UpdateApplicationJSON(*pApplication)
				> Continuation % "Failed to save application state" % Auditor / [=, InProgressScope = InProgressScope]
				{
					_fOnInfo("Application settings were successfully changed");
					Auditor.f_Info("Updated application settings (No restart required)");
					Continuation.f_SetResult();
				}
			;
			return Continuation;
		}
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, *pApplication, EEncryptOperation_Open, false)
			> [=, pApplication = *pApplication](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					Continuation.f_SetException(Auditor.f_Exception(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr())));
					return;
				}
				if (pApplication->m_bDeleted)
				{
					Continuation.f_SetException(Auditor.f_Exception("Application has been deleted, aborting"));
					return;
				}
				
				_fOnInfo("Stopping old application");
				pApplication->f_Stop(false) > [=](TCAsyncResult<uint32> &&_ExitStatus)
					{
						CStr Error = fp_GetApplicationStopErrors(_ExitStatus, pApplication->m_Name);
						
						if (!Error.f_IsEmpty())
						{
							_fOnInfo(Error);
							Auditor.f_Warning(Error);
						}
						
						if (!_ExitStatus)
						{
							Continuation.f_SetException(Auditor.f_Exception("Failed to exit old application, aborting update"));
							return;
						}
						
						if (pApplication->m_bDeleted)
						{
							Continuation.f_SetException(Auditor.f_Exception("Application has been deleted, aborting"));
							return;
						}

						pApplication->m_Settings = NewSettings;
						if (ChangedSettings & (EApplicationSetting_VersionManagerApplication | EApplicationSetting_UpdateGroup))
							fp_RemoteAppInfoChanged(pApplication);
						
						_fOnInfo("Saving application state and update application files");
						fg_Dispatch
							(
								mp_FileActor
								, [=, Directory = pApplication->f_GetDirectory(), InProgressScope = InProgressScope]()
								{
									fsp_CreateApplicationUserGroup(NewSettings, _fOnInfo, Directory);
									fsp_UpdateApplicationFiles(Directory, pApplication, pApplication->m_Files);
								}
							)
							+ fp_UpdateApplicationJSON(pApplication)
							> [=](TCAsyncResult<void> &&_Result, TCAsyncResult<void> &&_UpdateJSONResults)
							{
								bool bError = false;
								if (!_Result)
								{
									bError = true;
									Continuation.f_SetException(Auditor.f_Exception(fg_Format("Failed to update application files: {}", _Result.f_GetExceptionStr())));
								}
								
								if (!_UpdateJSONResults)
								{
									bError = true;
									auto Excption = Auditor.f_Exception(fg_Format("Failed to save application state: {}", _UpdateJSONResults.f_GetExceptionStr()));
									if (!Continuation.f_IsSet())
										Continuation.f_SetException(Excption);
								}
								else
									_fOnInfo("Application state successfully stored, so any changes will persist");
								
								if (bError)
									return;
								
								if (pApplication->m_bDeleted)
								{
									Continuation.f_SetException(Auditor.f_Exception("Application has been deleted, aborting"));
									return;
								}
									
								_fOnInfo("Launching applicaion with changed settings");
								fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
									> Continuation % "Failed to launch app. Will retry periodically." % Auditor / [=, InProgressScope = InProgressScope]
									{
										_fOnInfo("Application settings were successfully changed");
										Auditor.f_Info("Updated application settings");
										Continuation.f_SetResult();
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
}
