// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NAppManager
{
	CStr CAppManagerActor::fp_TransformUserGroup(CStr const &_UserName) const
	{
		return mp_pUniqueUserGroup->f_TransformUserGroup(_UserName);
	}

	CStr CAppManagerActor::fp_GetRunAsUser(CApplicationSettings const &_Settings) const
	{
		return mp_pUniqueUserGroup->f_GetUser(_Settings.m_RunAsUser);
	}

	CStr CAppManagerActor::fp_GetRunAsGroup(CApplicationSettings const &_Settings) const
	{
		return mp_pUniqueUserGroup->f_GetGroup(_Settings.m_RunAsGroup);
	}

	bool CAppManagerActor::CApplicationSettings::f_ParseSettings(CEJsonSorted const &_Params, EApplicationSetting &o_ChangedSettings, CStr &o_Error, bool _bRelaxed)
	{
		if (auto *pValue = _Params.f_GetMember("SelfUpdateSource"))
		{
			o_ChangedSettings |= EApplicationSetting_SelfUpdateSource;
			m_bSelfUpdateSource = pValue->f_Boolean();
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateGroup"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateGroup;
			m_UpdateGroup = pValue->f_String();
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
			m_EncryptionFileSystem = "xfs";
		}
		
		if (auto *pValue = _Params.f_GetMember("ParentApplication"))
		{
			o_ChangedSettings |= EApplicationSetting_ParentApplication;
			m_ParentApplication = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("Dependencies"))
		{
			o_ChangedSettings |= EApplicationSetting_Dependencies;
			m_Dependencies.f_Clear();
			if (pValue->f_IsArray())
			{
				for (auto &Dependency : pValue->f_Array())
					m_Dependencies[Dependency.f_String()];
			}
		}

		if (auto *pValue = _Params.f_GetMember("StopOnDependencyFailure"))
		{
			o_ChangedSettings |= EApplicationSetting_StopOnDependencyFailure;
			m_bStopOnDependencyFailure = pValue->f_Boolean();
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
#ifdef DPlatformFamily_Windows
		if (auto *pValue = _Params.f_GetMember("RunAsUserPassword"))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUserPassword;
			m_RunAsUserPassword = pValue->f_String();
		}
#endif
		
		if (auto *pValue = _Params.f_GetMember("RunAsGroup"))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsGroup;
			m_RunAsGroup = pValue->f_String();
		}

		if (auto *pValue = _Params.f_GetMember("RunAsUserHasShell"))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUserHasShell;
			m_bRunAsUserHasShell = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("LaunchInProcess"))
		{
			o_ChangedSettings |= EApplicationSetting_LaunchInProcess;
			m_bLaunchInProcess = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("BackupIncludeWildcards"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupIncludeWildcards;
			m_Backup_IncludeWildcards.f_Clear();
			for (auto &Wildcard : pValue->f_Object())
			{
				auto &Destination = m_Backup_IncludeWildcards[Wildcard.f_Name()];
				if (Wildcard.f_Value().f_IsNull())
					Destination = CDirectoryManifestConfig::CDestination{};
				else
					Destination = Wildcard.f_Value().f_String();
			}
		}

		if (auto *pValue = _Params.f_GetMember("BackupExcludeWildcards"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupExcludeWildcards;
			m_Backup_ExcludeWildcards.f_Clear();
			for (auto &Wildcard : pValue->f_Array())
				m_Backup_ExcludeWildcards[Wildcard.f_String()];
		}

		if (auto *pValue = _Params.f_GetMember("BackupAddSyncFlagsWildcards"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupAddSyncFlagsWildcards;
			m_Backup_AddSyncFlagsWildcards.f_Clear();
			for (auto &Wildcard : pValue->f_Object())
				m_Backup_AddSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
		}

		if (auto *pValue = _Params.f_GetMember("BackupRemoveSyncFlagsWildcards"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupRemoveSyncFlagsWildcards;
			m_Backup_RemoveSyncFlagsWildcards.f_Clear();
			for (auto &Wildcard : pValue->f_Object())
				m_Backup_RemoveSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
		}
		
		if (auto *pValue = _Params.f_GetMember("BackupNewBackupIntervalHours"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupNewBackupInterval;
			m_Backup_NewBackupInterval = CTimeSpanConvert::fs_CreateSpanFromHours(pValue->f_Float());
		}

		if (auto *pValue = _Params.f_GetMember("BackupEnabled"))
		{
			o_ChangedSettings |= EApplicationSetting_BackupEnabled;
			m_bBackupEnabled = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("DistributedApp"))
		{
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
			m_bDistributedApp = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("AutoUpdate"))
		{
			o_ChangedSettings |= EApplicationSetting_AutoUpdate;
			m_bAutoUpdate = pValue->f_Boolean();
		}

		if (auto *pValue = _Params.f_GetMember("UpdateTags"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateTags;
			m_UpdateTags.f_Clear();
			for (auto &TagJson : pValue->f_Array())
			{
				auto &Tag = TagJson.f_String();
				if (!CVersionManager::fs_IsValidTag(Tag))
				{
					o_Error = fg_Format("'{}' is not a valid tag", Tag);
					return false;
				}
				m_UpdateTags[Tag];
			}
		}
		
		if (auto *pValue = _Params.f_GetMember("UpdateBranches"))
		{
			o_ChangedSettings |= EApplicationSetting_UpdateBranches;
			m_UpdateBranches.f_Clear();
			for (auto &BranchJson : pValue->f_Array())
			{
				auto &Branch = BranchJson.f_String();
				if (!CVersionManager::fs_IsValidBranch(Branch, true))
				{
					o_Error = fg_Format("'{}' is not a valid branch or wildcard", Branch);
					return false;
				}
				m_UpdateBranches[Branch];
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
#ifdef DPlatformFamily_Windows
		if (_ChangedSettings & EApplicationSetting_RunAsUserPassword)
			m_RunAsUserPassword = _Source.m_RunAsUserPassword;
#endif
		if (_ChangedSettings & EApplicationSetting_RunAsGroup)
			m_RunAsGroup = _Source.m_RunAsGroup;
		if (_ChangedSettings & EApplicationSetting_RunAsUserHasShell)
			m_bRunAsUserHasShell = _Source.m_bRunAsUserHasShell;

		if (_ChangedSettings & EApplicationSetting_BackupIncludeWildcards)
			m_Backup_IncludeWildcards = _Source.m_Backup_IncludeWildcards;
		if (_ChangedSettings & EApplicationSetting_BackupExcludeWildcards)
			m_Backup_ExcludeWildcards = _Source.m_Backup_ExcludeWildcards;
		if (_ChangedSettings & EApplicationSetting_BackupAddSyncFlagsWildcards)
			m_Backup_AddSyncFlagsWildcards = _Source.m_Backup_AddSyncFlagsWildcards;
		if (_ChangedSettings & EApplicationSetting_BackupRemoveSyncFlagsWildcards)
			m_Backup_RemoveSyncFlagsWildcards = _Source.m_Backup_RemoveSyncFlagsWildcards;
		if (_ChangedSettings & EApplicationSetting_BackupNewBackupInterval)
			m_Backup_NewBackupInterval = _Source.m_Backup_NewBackupInterval;
		if (_ChangedSettings & EApplicationSetting_BackupEnabled)
			m_bBackupEnabled = _Source.m_bBackupEnabled;

		if (_ChangedSettings & EApplicationSetting_DistributedApp)
			m_bDistributedApp = _Source.m_bDistributedApp;
		if (_ChangedSettings & EApplicationSetting_VersionManagerApplication)
			m_VersionManagerApplication = _Source.m_VersionManagerApplication;
		if (_ChangedSettings & EApplicationSetting_AutoUpdate)
			m_bAutoUpdate = _Source.m_bAutoUpdate;
		if (_ChangedSettings & EApplicationSetting_UpdateTags)
			m_UpdateTags = _Source.m_UpdateTags;
		if (_ChangedSettings & EApplicationSetting_UpdateBranches)
			m_UpdateBranches = _Source.m_UpdateBranches;
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
		if (_ChangedSettings & EApplicationSetting_UpdateGroup)
			m_UpdateGroup = _Source.m_UpdateGroup;
		if (_ChangedSettings & EApplicationSetting_Dependencies)
			m_Dependencies = _Source.m_Dependencies;
		if (_ChangedSettings & EApplicationSetting_StopOnDependencyFailure)
			m_bStopOnDependencyFailure = _Source.m_bStopOnDependencyFailure;
		if (_ChangedSettings & EApplicationSetting_LaunchInProcess)
			m_bLaunchInProcess = _Source.m_bLaunchInProcess;
	}

	bool CAppManagerActor::CApplicationSettings::f_Validate(CStr &o_Error) const
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
#ifdef DPlatformFamily_Windows
			else if (!m_RunAsUserPassword.f_IsEmpty())
				return fError("For self update you cannot specify run as user password");
#endif
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
	
	auto CAppManagerActor::CApplicationSettings::f_ChangedSettings(CApplicationSettings const &_Other) const -> EApplicationSetting
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
#ifdef DPlatformFamily_Windows
		if (m_RunAsUserPassword != _Other.m_RunAsUserPassword)
			ChangedSettings |= EApplicationSetting_RunAsUserPassword;
#endif
		if (m_RunAsGroup != _Other.m_RunAsGroup)
			ChangedSettings |= EApplicationSetting_RunAsGroup;
		if (m_bRunAsUserHasShell != _Other.m_bRunAsUserHasShell)
			ChangedSettings |= EApplicationSetting_RunAsUserHasShell;

		if (m_Backup_IncludeWildcards != _Other.m_Backup_IncludeWildcards)
			ChangedSettings |= EApplicationSetting_BackupIncludeWildcards;
		if (m_Backup_ExcludeWildcards != _Other.m_Backup_ExcludeWildcards)
			ChangedSettings |= EApplicationSetting_BackupExcludeWildcards;
		if (m_Backup_AddSyncFlagsWildcards != _Other.m_Backup_AddSyncFlagsWildcards)
			ChangedSettings |= EApplicationSetting_BackupAddSyncFlagsWildcards;
		if (m_Backup_RemoveSyncFlagsWildcards != _Other.m_Backup_RemoveSyncFlagsWildcards)
			ChangedSettings |= EApplicationSetting_BackupRemoveSyncFlagsWildcards;
		if (m_Backup_NewBackupInterval != _Other.m_Backup_NewBackupInterval)
			ChangedSettings |= EApplicationSetting_BackupNewBackupInterval;
		if (m_bBackupEnabled != _Other.m_bBackupEnabled)
			ChangedSettings |= EApplicationSetting_BackupEnabled;
			
		if (m_bDistributedApp != _Other.m_bDistributedApp)
			ChangedSettings |= EApplicationSetting_DistributedApp;
		if (m_VersionManagerApplication != _Other.m_VersionManagerApplication)
			ChangedSettings |= EApplicationSetting_VersionManagerApplication;
		if (m_bAutoUpdate != _Other.m_bAutoUpdate)
			ChangedSettings |= EApplicationSetting_AutoUpdate;
		if (m_UpdateTags != _Other.m_UpdateTags)
			ChangedSettings |= EApplicationSetting_UpdateTags;
		if (m_UpdateBranches != _Other.m_UpdateBranches)
			ChangedSettings |= EApplicationSetting_UpdateBranches;
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
		if (m_UpdateGroup != _Other.m_UpdateGroup)
			ChangedSettings |= EApplicationSetting_UpdateGroup;
		if (m_Dependencies != _Other.m_Dependencies)
			ChangedSettings |= EApplicationSetting_Dependencies;
		if (m_bStopOnDependencyFailure != _Other.m_bStopOnDependencyFailure)
			ChangedSettings |= EApplicationSetting_StopOnDependencyFailure;
		if (m_bLaunchInProcess != _Other.m_bLaunchInProcess)
			ChangedSettings |= EApplicationSetting_LaunchInProcess;

		return ChangedSettings;
	}

	void CAppManagerActor::CApplicationSettings::f_FromInterfaceAdd(CAppManagerInterface::CApplicationAdd const &_Settings, EApplicationSetting &o_ChangedSettings)
	{
		if (!_Settings.m_ParentApplication.f_IsEmpty())
		{
			o_ChangedSettings |= EApplicationSetting_ParentApplication;
			m_ParentApplication = _Settings.m_ParentApplication;
		}

		bool bStorageSpecified = false;
		if (!_Settings.m_EncryptionStorage.f_IsEmpty())
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionStorage;
			m_EncryptionStorage = _Settings.m_EncryptionStorage;
			bStorageSpecified = true; 
		}
		
		if (!_Settings.m_EncryptionFileSystem.f_IsEmpty())
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionFileSystem;
			m_EncryptionFileSystem = _Settings.m_EncryptionFileSystem;
		}
		else if (bStorageSpecified)
		{
			o_ChangedSettings |= EApplicationSetting_EncryptionFileSystem;
			m_EncryptionFileSystem = "xfs";
		}
	}
	
	void CAppManagerActor::CApplicationSettings::f_FromInterfaceSettings(CAppManagerInterface::CApplicationSettings const &_Settings, EApplicationSetting &o_ChangedSettings)
	{
		if (_Settings.m_VersionManagerApplication)
		{
			m_VersionManagerApplication = *_Settings.m_VersionManagerApplication; 
			o_ChangedSettings |= EApplicationSetting_VersionManagerApplication;
		}
		if (_Settings.m_Executable)
		{
			m_Executable = *_Settings.m_Executable; 
			o_ChangedSettings |= EApplicationSetting_Executable;
		}
		if (_Settings.m_ExecutableParameters)
		{
			m_ExecutableParameters = *_Settings.m_ExecutableParameters; 
			o_ChangedSettings |= EApplicationSetting_ExecutableParameters;
		}
		if (_Settings.m_RunAsUser)
		{
			m_RunAsUser = *_Settings.m_RunAsUser; 
			o_ChangedSettings |= EApplicationSetting_RunAsUser;
		}
		if (_Settings.m_RunAsGroup)
		{
			m_RunAsGroup = *_Settings.m_RunAsGroup;
			o_ChangedSettings |= EApplicationSetting_RunAsGroup;
		}
		if (_Settings.m_bRunAsUserHasShell)
		{
			m_bRunAsUserHasShell = *_Settings.m_bRunAsUserHasShell;
			o_ChangedSettings |= EApplicationSetting_RunAsUserHasShell;
		}

		if (_Settings.m_Backup_IncludeWildcards)
		{
			m_Backup_IncludeWildcards = *_Settings.m_Backup_IncludeWildcards; 
			o_ChangedSettings |= EApplicationSetting_BackupIncludeWildcards;
		}
		if (_Settings.m_Backup_ExcludeWildcards)
		{
			m_Backup_ExcludeWildcards = *_Settings.m_Backup_ExcludeWildcards; 
			o_ChangedSettings |= EApplicationSetting_BackupExcludeWildcards;
		}
		if (_Settings.m_Backup_AddSyncFlagsWildcards)
		{
			m_Backup_AddSyncFlagsWildcards = *_Settings.m_Backup_AddSyncFlagsWildcards; 
			o_ChangedSettings |= EApplicationSetting_BackupAddSyncFlagsWildcards;
		}
		if (_Settings.m_Backup_RemoveSyncFlagsWildcards)
		{
			m_Backup_RemoveSyncFlagsWildcards = *_Settings.m_Backup_RemoveSyncFlagsWildcards; 
			o_ChangedSettings |= EApplicationSetting_BackupRemoveSyncFlagsWildcards;
		}
		if (_Settings.m_Backup_NewBackupInterval)
		{
			m_Backup_NewBackupInterval = *_Settings.m_Backup_NewBackupInterval; 
			o_ChangedSettings |= EApplicationSetting_BackupNewBackupInterval;
		}
		if (_Settings.m_bBackupEnabled)
		{
			m_bBackupEnabled = *_Settings.m_bBackupEnabled; 
			o_ChangedSettings |= EApplicationSetting_BackupEnabled;
		}
		
		if (_Settings.m_bDistributedApp)
		{
			m_bDistributedApp = *_Settings.m_bDistributedApp; 
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
		}
		if (_Settings.m_bAutoUpdate)
		{
			m_bAutoUpdate = *_Settings.m_bAutoUpdate;
			o_ChangedSettings |= EApplicationSetting_AutoUpdate;
		}
		if (_Settings.m_UpdateTags)
		{
			m_UpdateTags = *_Settings.m_UpdateTags;
			o_ChangedSettings |= EApplicationSetting_UpdateTags;
		}
		if (_Settings.m_UpdateBranches)
		{
			m_UpdateBranches = *_Settings.m_UpdateBranches;
			o_ChangedSettings |= EApplicationSetting_UpdateBranches;
		}
		if (_Settings.m_UpdateScriptPreUpdate)
		{
			m_UpdateScripts.m_PreUpdate = *_Settings.m_UpdateScriptPreUpdate; 
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PreUpdate;
		}
		if (_Settings.m_UpdateScriptPostUpdate)
		{
			m_UpdateScripts.m_PostUpdate = *_Settings.m_UpdateScriptPostUpdate; 
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PostUpdate;
		}
		if (_Settings.m_UpdateScriptPostLaunch)
		{
			m_UpdateScripts.m_PostLaunch = *_Settings.m_UpdateScriptPostLaunch; 
			o_ChangedSettings |= EApplicationSetting_UpdateScript_PostLaunch;
		}
		if (_Settings.m_UpdateScriptOnError)
		{
			m_UpdateScripts.m_OnError = *_Settings.m_UpdateScriptOnError; 
			o_ChangedSettings |= EApplicationSetting_UpdateScript_OnError;
		}
		if (_Settings.m_bSelfUpdateSource)
		{
			m_bSelfUpdateSource = *_Settings.m_bSelfUpdateSource; 
			o_ChangedSettings |= EApplicationSetting_SelfUpdateSource;
		}
		if (_Settings.m_UpdateGroup)
		{
			m_UpdateGroup = *_Settings.m_UpdateGroup; 
			o_ChangedSettings |= EApplicationSetting_UpdateGroup;
		}
		if (_Settings.m_Dependencies)
		{
			m_Dependencies = *_Settings.m_Dependencies; 
			o_ChangedSettings |= EApplicationSetting_Dependencies;
		}
		if (_Settings.m_bStopOnDependencyFailure)
		{
			m_bStopOnDependencyFailure = *_Settings.m_bStopOnDependencyFailure; 
			o_ChangedSettings |= EApplicationSetting_StopOnDependencyFailure;
		}
		if (_Settings.m_bLaunchInProcess)
		{
			m_bLaunchInProcess = *_Settings.m_bLaunchInProcess;
			o_ChangedSettings |= EApplicationSetting_LaunchInProcess;
		}
	}
	
	void CAppManagerActor::CApplicationSettings::f_FromVersionInfo(CVersionManager::CVersionInformation const &_Info, EApplicationSetting &o_ChangedSettings)
	{
		auto &ExtraInfo = _Info.m_ExtraInfo;
		if (auto *pValue = ExtraInfo.f_GetMember("Executable", EJsonType_String))
		{
			o_ChangedSettings |= EApplicationSetting_Executable;
			m_Executable = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("RunAsUser", EJsonType_String))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUser;
			m_RunAsUser = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("RunAsGroup", EJsonType_String))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsGroup;
			m_RunAsGroup = pValue->f_String();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("RunAsUserHasShell", EJsonType_Boolean))
		{
			o_ChangedSettings |= EApplicationSetting_RunAsUserHasShell;
			m_bRunAsUserHasShell = pValue->f_Boolean();
		}

		if (auto *pValue = ExtraInfo.f_GetMember("Backup", EJsonType_Object))
		{
			auto &BackupJson = *pValue;
			
			if (auto pValue = BackupJson.f_GetMember("IncludeWildcards", EJsonType_Array))
			{
				o_ChangedSettings |= EApplicationSetting_BackupIncludeWildcards;
				m_Backup_IncludeWildcards.f_Clear();
				for (auto &Wildcard : pValue->f_Object())
				{
					auto &Destination = m_Backup_IncludeWildcards[Wildcard.f_Name()];
					if (Wildcard.f_Value().f_IsNull())
						Destination = CDirectoryManifestConfig::CDestination{};
					else
						Destination = Wildcard.f_Value().f_String();
				}
			}
			if (auto pValue = BackupJson.f_GetMember("ExcludeWildcards", EJsonType_Array))
			{
				o_ChangedSettings |= EApplicationSetting_BackupExcludeWildcards;
				m_Backup_ExcludeWildcards.f_Clear();
				for (auto &Wildcard : pValue->f_Array())
					m_Backup_ExcludeWildcards[Wildcard.f_String()];
			}
			if (auto pValue = BackupJson.f_GetMember("AddSyncFlagsWildcards", EJsonType_Object))
			{
				o_ChangedSettings |= EApplicationSetting_BackupAddSyncFlagsWildcards;
				m_Backup_AddSyncFlagsWildcards.f_Clear();
				for (auto &Wildcard : pValue->f_Object())
					m_Backup_AddSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
			}
			if (auto pValue = BackupJson.f_GetMember("RemoveSyncFlagsWildcards", EJsonType_Object))
			{
				o_ChangedSettings |= EApplicationSetting_BackupRemoveSyncFlagsWildcards;
				m_Backup_RemoveSyncFlagsWildcards.f_Clear();
				for (auto &Wildcard : pValue->f_Object())
					m_Backup_RemoveSyncFlagsWildcards[Wildcard.f_Name()] = CDirectoryManifestFile::fs_ParseSyncFlags(Wildcard.f_Value());
			}
			{
				auto pValue = BackupJson.f_GetMember("NewBackupInterval", EJsonType_Float);
				if (!pValue)
					pValue = BackupJson.f_GetMember("NewBackupInterval", EJsonType_Integer);
				if (pValue)
				{
					o_ChangedSettings |= EApplicationSetting_BackupNewBackupInterval;
					m_Backup_NewBackupInterval = CTimeSpanConvert::fs_CreateSpanFromHours(pValue->f_AsFloat());
				}
			}
		}

		if (auto *pValue = ExtraInfo.f_GetMember("ExecutableParams", EJsonType_Array))
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
		
		if (auto *pValue = ExtraInfo.f_GetMember("DistributedApp", EJsonType_Boolean))
		{
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
			m_bDistributedApp = pValue->f_Boolean();
		}
		
		if (auto *pValue = ExtraInfo.f_GetMember("Dependencies"))
		{
			o_ChangedSettings |= EApplicationSetting_Dependencies;
			m_Dependencies.f_Clear();
			if (pValue->f_IsArray())
			{
				for (auto &Dependency : pValue->f_Array())
					m_Dependencies[Dependency.f_String()];
			}
		}

		if (auto *pValue = ExtraInfo.f_GetMember("StopOnDependencyFailure"))
		{
			o_ChangedSettings |= EApplicationSetting_StopOnDependencyFailure;
			m_bStopOnDependencyFailure = pValue->f_Boolean();
		}
	}
}
