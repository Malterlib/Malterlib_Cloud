// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	bool CAppManagerActor::CApplicationSettings::f_ParseSettings(CEJSON const &_Params, EApplicationSetting &o_ChangedSettings, CStr &o_Error, bool _bRelaxed)
	{
		if (auto *pValue = _Params.f_GetMember("SelfUpdateSource"))
		{
			o_ChangedSettings |= EApplicationSetting_SelfUpdateSource;
			m_bSelfUpdateSource = pValue->f_Boolean();
		}
		
		bool bStorageSpecified = false;
		if (auto *pValue = _Params.f_GetMember("EncryptionStorage"))
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionStorage;
			m_EncryptionStorage = pValue->f_String();
			bStorageSpecified = !m_EncryptionStorage.f_IsEmpty(); 
		}
		
		if (auto *pValue = _Params.f_GetMember("EncryptionFileSystem"))
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionFileSystem;
			m_EncryptionFileSystem = pValue->f_String();
		}
		else if (bStorageSpecified)
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionFileSystem;
			m_EncryptionFileSystem = "zfs";
		}
		
		if (auto *pValue = _Params.f_GetMember("ParentApplication"))
		{
			o_ChangedSettings |= EApplicationSetting_ParentApplication;
			m_ParentApplication = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("VersionManagerApplication"))
		{
			o_ChangedSettings |= EApplicationSetting_VersionManagerApplication;
			m_VersionManagerApplication = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("Executable"))
		{
			o_ChangedSettings |= EApplicationSetting_Executable;
			m_Executable = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("ExecutableParameters"))
		{
			if (!_bRelaxed || !m_bSelfUpdateSource)
			{
				o_ChangedSettings |= EApplicationSetting_ExecutableParameters;
				m_ExecutableParameters.f_Clear();
				for (auto &Parameter : pValue->f_Array())
					m_ExecutableParameters.f_Insert(Parameter.f_String());
			}
		}
		
		if (auto *pValue = _Params.f_GetMember("RunAsUser"))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUser;
			m_RunAsUser = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("RunAsGroup"))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsGroup;
			m_RunAsGroup = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("AutoUpdateTags"))
		{
			o_ChangedSettings |= EApplicationSetting_AutoUpdateTags;
			m_bAutoUpdate = false;
			m_AutoUpdateTags.f_Clear();
			if (pValue->f_IsArray())
			{
				m_bAutoUpdate = true;
				for (auto &TagJSON : pValue->f_Array())
				{
					auto &Tag = TagJSON.f_String();
					if (!CVersionManager::fs_IsValidTag(Tag))
					{
						o_Error = fg_Format("'{}' is not a valid tag", Tag);
						return false;
					}
					m_AutoUpdateTags[Tag];
				}
			}
		}
		
		if (auto *pValue = _Params.f_GetMember("AutoUpdateBranches"))
		{
			o_ChangedSettings |= EApplicationSetting_AutoUpdateBranches;
			m_AutoUpdateBranches.f_Clear();
			for (auto &BranchJSON : pValue->f_Array())
			{
				auto &Branch = BranchJSON.f_String();
				if (!CVersionManager::fs_IsValidBranch(Branch, true))
				{
					o_Error = fg_Format("'{}' is not a valid branch or wildcard", Branch);
					return false;
				}
				m_AutoUpdateBranches[Branch];
			}
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateScript_PreUpdate"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PreUpdate;
			m_UpdateScripts.m_PreUpdate = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateScript_PostUpdate"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PostUpdate;
			m_UpdateScripts.m_PostUpdate = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateScript_PostLaunch"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PostLaunch;
			m_UpdateScripts.m_PostLaunch = pValue->f_String();
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateScript_OnError"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateScript_OnError;
			m_UpdateScripts.m_OnError = pValue->f_String();
		}
		return true;
	}
	
	void CAppManagerActor::CApplicationSettings::f_ApplySettings(EApplicationSetting _ChangedSettings, CApplicationSettings const &_Source)
	{
		if (_ChangedSettings & EApplicationSetting_EncryptionStorage)
			m_EncryptionStorage = _Source.m_EncryptionStorage;
		if (_ChangedSettings & EApplicationSetting_EncryptionFileSystem)
			m_EncryptionFileSystem = _Source.m_EncryptionFileSystem;
		if (_ChangedSettings & EApplicationSetting_ParentApplication)
			m_ParentApplication = _Source.m_ParentApplication;
		if (_ChangedSettings & EApplicationSetting_Executable)
			m_Executable = _Source.m_Executable;
		if (_ChangedSettings & EApplicationSetting_ExecutableParameters)
			m_ExecutableParameters = _Source.m_ExecutableParameters;
		if (_ChangedSettings & EApplicationSetting_RunAsUser)
			m_RunAsUser = _Source.m_RunAsUser;
		if (_ChangedSettings & EApplicationSetting_RunAsGroup)
			m_RunAsGroup = _Source.m_RunAsGroup;
		if (_ChangedSettings & EApplicationSetting_VersionManagerApplication)
			m_VersionManagerApplication = _Source.m_VersionManagerApplication;
		if (_ChangedSettings & EApplicationSetting_AutoUpdateTags)
		{
			m_bAutoUpdate = _Source.m_bAutoUpdate;
			m_AutoUpdateTags = _Source.m_AutoUpdateTags;
		}
		if (_ChangedSettings & EApplicationSetting_AutoUpdateBranches)
			m_AutoUpdateBranches = _Source.m_AutoUpdateBranches;
		if (_ChangedSettings & EApplicationSetting_UpdateScript_PreUpdate)
			m_UpdateScripts.m_PreUpdate = _Source.m_UpdateScripts.m_PreUpdate;
		if (_ChangedSettings & EApplicationSetting_UpdateScript_PostUpdate)
			m_UpdateScripts.m_PostUpdate = _Source.m_UpdateScripts.m_PostUpdate;
		if (_ChangedSettings & EApplicationSetting_UpdateScript_PostLaunch)
			m_UpdateScripts.m_PostLaunch = _Source.m_UpdateScripts.m_PostLaunch;
		if (_ChangedSettings & EApplicationSetting_UpdateScript_OnError)
			m_UpdateScripts.m_OnError = _Source.m_UpdateScripts.m_OnError;
		if (_ChangedSettings & EApplicationSetting_SelfUpdateSource)
			m_bSelfUpdateSource = _Source.m_bSelfUpdateSource;
	}

	bool CAppManagerActor::CApplicationSettings::f_Validate(CStr &o_Error)
	{
		auto fError = [&](CStr const &_Error)
			{
				o_Error = _Error;
				return false;
			}
		;
		if (m_bSelfUpdateSource)
		{
			if (!m_EncryptionStorage.f_IsEmpty())
				return fError("For self update you cannot specify encryption storage");
			else if (!m_EncryptionFileSystem.f_IsEmpty())
				return fError("For self update you cannot specify encryption file system");
			else if (!m_Executable.f_IsEmpty())
				return fError("For self update you cannot specify executable");
			else if (!m_ExecutableParameters.f_IsEmpty())
				return fError("For self update you cannot specify executable parameters");
			else if (!m_RunAsUser.f_IsEmpty())
				return fError("For self update you cannot specify run as user");
 			else if (!m_RunAsGroup.f_IsEmpty())
				return fError("For self update you cannot specify run as group");
		}
		else
		{
			if (!m_ParentApplication.f_IsEmpty())
			{
				if (!m_EncryptionStorage.f_IsEmpty())
					return fError("For child application you cannot specify encryption storage");
				else if (!m_EncryptionFileSystem.f_IsEmpty())
					return fError("For child application you cannot specify encryption file system");
			}
		}
		
		return true;
	}
	
	auto CAppManagerActor::CApplicationSettings::f_ChangedSettings(CApplicationSettings const &_Other) -> EApplicationSetting 
	{
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		if (m_EncryptionStorage != _Other.m_EncryptionStorage)
			ChangedSettings |= EApplicationSetting_EncryptionStorage;
		if (m_EncryptionFileSystem != _Other.m_EncryptionFileSystem)
			ChangedSettings |= EApplicationSetting_EncryptionFileSystem;
		if (m_ParentApplication != _Other.m_ParentApplication)
			ChangedSettings |= EApplicationSetting_ParentApplication;
		if (m_Executable != _Other.m_Executable)
			ChangedSettings |= EApplicationSetting_Executable;
		if (m_ExecutableParameters != _Other.m_ExecutableParameters)
			ChangedSettings |= EApplicationSetting_ExecutableParameters;
		if (m_RunAsUser != _Other.m_RunAsUser)
			ChangedSettings |= EApplicationSetting_RunAsUser;
		if (m_RunAsGroup != _Other.m_RunAsGroup)
			ChangedSettings |= EApplicationSetting_RunAsGroup;
		if (m_VersionManagerApplication != _Other.m_VersionManagerApplication)
			ChangedSettings |= EApplicationSetting_VersionManagerApplication;
		if (m_bAutoUpdate != _Other.m_bAutoUpdate || m_AutoUpdateTags != _Other.m_AutoUpdateTags)
			ChangedSettings |= EApplicationSetting_AutoUpdateTags;
		if (m_AutoUpdateBranches != _Other.m_AutoUpdateBranches)
			ChangedSettings |= EApplicationSetting_AutoUpdateBranches;
		if (m_UpdateScripts.m_PreUpdate != _Other.m_UpdateScripts.m_PreUpdate)
			ChangedSettings |= EApplicationSetting_UpdateScript_PreUpdate;
		if (m_UpdateScripts.m_PostUpdate != _Other.m_UpdateScripts.m_PostUpdate)
			ChangedSettings |= EApplicationSetting_UpdateScript_PostUpdate;
		if (m_UpdateScripts.m_PostLaunch != _Other.m_UpdateScripts.m_PostLaunch)
			ChangedSettings |= EApplicationSetting_UpdateScript_PostLaunch;
		if (m_UpdateScripts.m_OnError != _Other.m_UpdateScripts.m_OnError)
			ChangedSettings |= EApplicationSetting_UpdateScript_OnError;
		if (m_bSelfUpdateSource != _Other.m_bSelfUpdateSource)
			ChangedSettings |= EApplicationSetting_SelfUpdateSource;
		return ChangedSettings;
	}
	
	void CAppManagerActor::CApplicationSettings::f_FromVersionInfo(CVersionManager::CVersionInformation const &_Info, EApplicationSetting &o_ChangedSettings)
	{
		auto &ExtraInfo = _Info.m_ExtraInfo;
		if (auto *pValue = ExtraInfo.f_GetMember("Executable", EJSONType_String))
		{
			o_ChangedSettings |= EApplicationSetting_Executable;
			m_Executable = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("RunAsUser", EJSONType_String))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUser;
			m_RunAsUser = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("RunAsGroup", EJSONType_String))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsGroup;
			m_RunAsGroup = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("ExecutableParams", EJSONType_Array))
		{
			o_ChangedSettings |= EApplicationSetting_ExecutableParameters;
			m_ExecutableParameters.f_Clear();
			for (auto &Param : pValue->f_Array())
			{
				if (!Param.f_IsString())
					continue;
				m_ExecutableParameters.f_Insert(Param.f_String());
			}
		}
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_ChangeApplicationSettings(CEJSON const &_Params)
	{
		CStr Name = _Params["Name"].f_String();
		
		auto *pApplication = mp_Applications.f_FindEqual(Name);
		if (!pApplication)
			return DMibErrorInstance(fg_Format("No such application '{}'", Name));
		auto &Application = **pApplication;

		CApplicationSettings Settings;
		EApplicationSetting ChangedSettings = EApplicationSetting_None;
		
		bool bUpdateFromVersionInfo = _Params["UpdateFromVersionInfo"].f_Boolean();
		bool bForce = _Params["Force"].f_Boolean();

		if (bUpdateFromVersionInfo)
		{
			if (!Application.m_LastInstalledVersion.f_IsValid())
				return DMibErrorInstance("Found no install from last version to get settings from");
			Settings.f_FromVersionInfo(Application.m_LastInstalledVersionInfo, ChangedSettings);
		}
		
		{
			CStr Error;
			if (!Settings.f_ParseSettings(_Params, ChangedSettings, Error, false))
				return DMibErrorInstance(Error);
		}

		auto NewSettings = Application.m_Settings;
		NewSettings.f_ApplySettings(ChangedSettings, Settings);	

		{
			CStr Error;
			if (!NewSettings.f_Validate(Error))
				return DMibErrorInstance(Error);
		}
		
 		ChangedSettings = Application.m_Settings.f_ChangedSettings(NewSettings);

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
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Command line command failed (change application settings): {}", _Error);
			}
		;
		
		if (ChangedSettings & EApplicationSetting_EncryptionStorage)
		{
			fLogError("Changing encryption storage is not supported");
			return Continuation;
		}
		if (ChangedSettings & EApplicationSetting_EncryptionFileSystem)
		{
			fLogError("Changing encryption file system is not supported");
			return Continuation;
		}
		if (ChangedSettings & EApplicationSetting_ParentApplication)
		{
			fLogError("Changing parent application is not supported");
			return Continuation;
		}
		if (ChangedSettings == EApplicationSetting_None && !bForce)
		{
			fLogInfo("No settings were changed. To updating of file permissions run with --force");
			Continuation.f_SetResult(fg_Move(*pResult));
			return Continuation;
		}
		
		if (Application.f_IsInProgress())
			return DMibErrorInstance("Operation already in progress for application");

		auto InProgressScope = Application.f_SetInProgress();
		
		if (!(ChangedSettings & EApplicationSetting_NeedUpdateSettings) && !bForce)
		{
			Application.m_Settings = NewSettings;
			fLogInfo("Saving application state");
			fp_UpdateApplicationJSON(*pApplication)
				> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_UpdateJSONResults)
				{
					if (!_UpdateJSONResults)
					{
						fLogError(fg_Format("Failed to save application state: {}", _UpdateJSONResults.f_GetExceptionStr()));
						return;
					}
					fLogInfo("Application settings were successfully changed");
					Continuation.f_SetResult(fg_Move(*pResult));
				}
			;
			return Continuation;
		}
		
		fg_ThisActor(this)(&CAppManagerActor::fp_ChangeEncryption, *pApplication, EEncryptOperation_Open, false)
			>[=, pApplication = *pApplication](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					fLogError(fg_Format("Failed to open encryption: {}", _Result.f_GetExceptionStr()));
					return;
				}
				if (pApplication->m_bDeleted)
				{
					fLogError("Application has been deleted, aborting");
					return;
				}
				
				fLogInfo("Stopping old application");
				pApplication->f_Stop(false) > [=](TCAsyncResult<uint32> &&_ExitStatus)
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

						pApplication->m_Settings = NewSettings;
						
						fLogInfo("Saving application state and update application files");
						fg_Dispatch
							(
								mp_FileActor
								, [=, Directory = pApplication->f_GetDirectory(), InProgressScope = InProgressScope]()
								{
									fsp_CreateApplicationUserGroup(NewSettings, fLogInfo, Directory);
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
									fLogError(fg_Format("Failed to update application files: {}", _Result.f_GetExceptionStr()));
								}
								if (!_UpdateJSONResults)
								{
									bError = true;
									fLogError(fg_Format("Failed to save application state: {}", _UpdateJSONResults.f_GetExceptionStr()));
								}
								else
									fLogInfo("Application state successfully stored, so any changes will persist");
								
								if (bError)
									return;
								
								if (pApplication->m_bDeleted)
								{
									fLogError("Application has been deleted, aborting");
									return;
								}
									
								fLogInfo("Launching applicaion with changed settings");
								fg_ThisActor(this)(&CAppManagerActor::fp_LaunchApp, pApplication, false)
									> [=, InProgressScope = InProgressScope](TCAsyncResult<void> &&_Result)
									{
										if (!_Result)
										{
											fLogError(fg_Format("Failed to launch app: {}. Will retry periodically.", _Result.f_GetExceptionStr()));
											return;
										}
										fLogInfo("Application settings were successfully changed");
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
}
