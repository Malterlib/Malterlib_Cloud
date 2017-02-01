// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_AppManager.h"

namespace NMib::NCloud
{
	CAppManagerInterface::CAppManagerInterface()
	{
		DMibPublishActorFunction(CAppManagerInterface::f_GetAvailableVersions);
		DMibPublishActorFunction(CAppManagerInterface::f_Add);
		DMibPublishActorFunction(CAppManagerInterface::f_Remove);
		DMibPublishActorFunction(CAppManagerInterface::f_Update);
		DMibPublishActorFunction(CAppManagerInterface::f_Start);
		DMibPublishActorFunction(CAppManagerInterface::f_Stop);
		DMibPublishActorFunction(CAppManagerInterface::f_Restart);
		DMibPublishActorFunction(CAppManagerInterface::f_ChangeSettings);
		DMibPublishActorFunction(CAppManagerInterface::f_GetInstalled);
	}

	DMibDistributedStreamImplement(CAppManagerInterface::CVersionIDAndPlatform);
	DMibDistributedStreamImplement(CAppManagerInterface::CVersionID);
	DMibDistributedStreamImplement(CAppManagerInterface::CVersionInformation);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationVersion);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationSettings);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationAdd);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationChangeSettings);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationInfo);
	DMibDistributedStreamImplement(CAppManagerInterface::CApplicationUpdate);
	
	CAppManagerInterface::~CAppManagerInterface() = default;

	static_assert(CVersionManager::EMinProtocolVersion <= 0x104, "");
	
	template <typename tf_CStream>
	void CAppManagerInterface::CVersionIDAndPlatform::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x104);
		CVersionManager::CVersionIDAndPlatform::f_Stream(_Stream);
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CVersionID::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x104);
		CVersionManager::CVersionID::f_Stream(_Stream);
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CVersionInformation::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x104);
		CVersionManager::CVersionInformation::f_Stream(_Stream);
	}
		
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationSettings::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_VersionManagerApplication;
		_Stream % m_Executable;
		_Stream % m_ExecutableParameters;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_AutoUpdateTags;
		_Stream % m_AutoUpdateBranches;
		_Stream % m_UpdateScriptPreUpdate;
		_Stream % m_UpdateScriptPostUpdate;
		_Stream % m_UpdateScriptPostLaunch;
		_Stream % m_UpdateScriptOnError;
		_Stream % m_bSelfUpdateSource;
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationAdd::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ParentApplication;
		_Stream % m_EncryptionStorage; 			
		_Stream % m_EncryptionFileSystem;
		_Stream % m_Version;
		_Stream % m_bForceOverwriteEncryption;
		_Stream % m_bForceInstall;
		_Stream % m_bSettingsFromVersionInfo;
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationChangeSettings::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_bUpdateFromVersionInfo;
		_Stream % m_bForce;
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Status;
		_Stream % m_EncryptionStorage;
		_Stream % m_EncryptionFileSystem;
		_Stream % m_ParentApplication;
		_Stream % m_Version;
		_Stream % m_VersionInfo;
		_Stream % m_VersionManagerApplication;
		_Stream % m_Executable;
		_Stream % m_Parameters;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_AutoUpdateTags;
		_Stream % m_AutoUpdateBranches;
		_Stream % m_UpdateScriptPreUpdate;
		_Stream % m_UpdateScriptPostUpdate;
		_Stream % m_UpdateScriptPostLaunch;
		_Stream % m_UpdateScriptOnError;
		_Stream % m_bSelfUpdateSource;
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationUpdate::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_RequireTags;
		_Stream % m_Platform;
		_Stream % m_Version;
		_Stream % m_bUpdateSettings;
		_Stream % m_bDryRun;
	}
	
	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationVersion::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_VersionID;
		_Stream % m_VersionInfo;
	}
}
