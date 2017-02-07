// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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

		if (auto *pValue = _Params.f_GetMember("DistributedApp"))
		{
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
			m_bDistributedApp = pValue->f_Boolean();
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
		if (_ChangedSettings & EApplicationSetting_DistributedApp)
			m_bDistributedApp = _Source.m_bDistributedApp;
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
		if (_ChangedSettings & EApplicationSetting_UpdateGroup)
			m_UpdateGroup = _Source.m_UpdateGroup;
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
 			else if (!m_RunAsGroup.f_IsEmpty())
				return fError("For self update you cannot specify run as group");
 			else if (m_bDistributedApp)
				return fError("For self update you cannot specify distributed app");
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
		if (m_RunAsGroup != _Other.m_RunAsGroup)
			ChangedSettings |= EApplicationSetting_RunAsGroup;
		if (m_bDistributedApp != _Other.m_bDistributedApp)
			ChangedSettings |= EApplicationSetting_DistributedApp;
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
		if (m_UpdateGroup != _Other.m_UpdateGroup)
			ChangedSettings |= EApplicationSetting_UpdateGroup;
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
		if (_Settings.m_bDistributedApp)
		{
			m_bDistributedApp = *_Settings.m_bDistributedApp; 
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
		}
		if (_Settings.m_AutoUpdateTags)
		{
			m_AutoUpdateTags = *_Settings.m_AutoUpdateTags;
			m_bAutoUpdate = !m_AutoUpdateTags.f_IsEmpty();
			o_ChangedSettings |= EApplicationSetting_AutoUpdateTags;
		}
		if (_Settings.m_AutoUpdateBranches)
		{
			m_AutoUpdateBranches = *_Settings.m_AutoUpdateBranches; 
			o_ChangedSettings |= EApplicationSetting_AutoUpdateBranches;
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
		
		if (auto *pValue = ExtraInfo.f_GetMember("DistributedApp", EJSONType_Boolean))
		{
			o_ChangedSettings |= EApplicationSetting_DistributedApp;
			m_bDistributedApp = pValue->f_Boolean();
		}
	}
}
