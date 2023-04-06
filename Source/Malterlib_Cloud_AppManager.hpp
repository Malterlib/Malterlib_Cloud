// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CAppManagerInterface::CVersionIDAndPlatform::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x105);
		CVersionManager::CVersionIDAndPlatform::f_Stream(_Stream);
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CVersionID::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x105);
		CVersionManager::CVersionID::f_Stream(_Stream);
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CVersionInformation::f_Stream(tf_CStream &_Stream)
	{
		DMibBinaryStreamVersion(_Stream, 0x105);
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
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddRunAsUserHasShell)
			_Stream % m_bRunAsUserHasShell;

		_Stream % m_Backup_IncludeWildcards;
		_Stream % m_Backup_ExcludeWildcards;
		_Stream % m_Backup_AddSyncFlagsWildcards;
		_Stream % m_Backup_RemoveSyncFlagsWildcards;
		_Stream % m_Backup_NewBackupInterval;

		_Stream % m_UpdateTags;
		_Stream % m_UpdateBranches;

		if (_Stream.f_GetVersion() >= EProtocolVersion_AddAutoUpdateAndExtendAppInfo)
			_Stream % m_bAutoUpdate;
		else if constexpr (tf_CStream::mc_bConsume)
		{
			if (m_UpdateTags)
				m_bAutoUpdate = !(*m_UpdateTags).f_IsEmpty();
		}

		_Stream % m_UpdateScriptPreUpdate;
		_Stream % m_UpdateScriptPostUpdate;
		_Stream % m_UpdateScriptPostLaunch;
		_Stream % m_UpdateScriptOnError;
		_Stream % m_UpdateGroup;
		_Stream % m_Dependencies;

		_Stream % m_bSelfUpdateSource;
		_Stream % m_bDistributedApp;
		_Stream % m_bStopOnDependencyFailure;
		_Stream % m_bBackupEnabled;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddLaunchInProcess)
			_Stream % m_bLaunchInProcess;
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
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddStatusSeverity)
			_Stream % m_StatusSeverity;
		_Stream % m_EncryptionStorage;
		_Stream % m_EncryptionFileSystem;
		_Stream % m_ParentApplication;

		if (_Stream.f_GetVersion() >= EProtocolVersion_HostIDInApplicationInfo)
			_Stream % m_HostID;

		_Stream % m_Version;
		_Stream % m_VersionInfo;
		_Stream % m_VersionManagerApplication;

		if (_Stream.f_GetVersion() >= EProtocolVersion_AddAutoUpdateAndExtendAppInfo)
		{
			_Stream % m_FailedVersion;
			_Stream % m_FailedVersionInfo;
			_Stream % m_FailedVersionError;

			_Stream % m_NewestUnconditionalVersion;
			_Stream % m_NewestUnconditionalVersionInfo;

			_Stream % m_WantVersion;
			_Stream % m_WantVersionInfo;
		}

		_Stream % m_Executable;
		_Stream % m_Parameters;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddRunAsUserHasShell)
			_Stream % m_bRunAsUserHasShell;

		_Stream % m_Backup_IncludeWildcards;
		_Stream % m_Backup_ExcludeWildcards;
		_Stream % m_Backup_AddSyncFlagsWildcards;
		_Stream % m_Backup_RemoveSyncFlagsWildcards;
		_Stream % m_Backup_NewBackupInterval;

		_Stream % m_UpdateTags;
		_Stream % m_UpdateBranches;

		if (_Stream.f_GetVersion() >= EProtocolVersion_AddAutoUpdateAndExtendAppInfo)
			_Stream % m_bAutoUpdate;
		else if constexpr (tf_CStream::mc_bConsume)
			m_bAutoUpdate = !m_UpdateTags.f_IsEmpty();

		_Stream % m_UpdateScriptPreUpdate;
		_Stream % m_UpdateScriptPostUpdate;
		_Stream % m_UpdateScriptPostLaunch;
		_Stream % m_UpdateScriptOnError;
		_Stream % m_UpdateGroup;
		_Stream % m_Dependencies;

		_Stream % m_bSelfUpdateSource;
		_Stream % m_bDistributedApp;
		_Stream % m_bStopOnDependencyFailure;
		_Stream % m_bBackupEnabled;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddLaunchInProcess)
			_Stream % m_bLaunchInProcess;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CUpdateNotification::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_Message;
		_Stream % m_VersionID;
		_Stream % m_VersionTime;
		_Stream % m_Stage;

		if (_Stream.f_GetVersion() >= EProtocolVersion_AddCoordinatedWait)
			_Stream % m_bCoordinateWait;

		if (_Stream.f_GetVersion() >= EProtocolVersion_ExtendUpdateNotification)
		{
			_Stream % m_UpdateID;
			_Stream % m_UpdateTime;
			_Stream % m_StartUpdateTime;
		}
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationUpdate::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_RequireTags;
		_Stream % m_Platform;
		_Stream % m_Version;
		_Stream % m_bUpdateSettings;
		_Stream % m_bDryRun;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddBypassCoordination)
			_Stream % m_bBypassCoordination;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationVersion::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_VersionID;
		_Stream % m_VersionInfo;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationChange_AddOrChangeInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Info;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationChange_Remove::f_Stream(tf_CStream &_Stream)
	{
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CApplicationChange_Status::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Status;
		_Stream % m_StatusSeverity;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::CChangeNotification::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_Change;
	}

	template <typename tf_CStream>
	void CAppManagerInterface::COnChangeNotificationParams::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Changes;
		_Stream % m_bInitial;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddFiltered)
			_Stream % m_bFiltered;
		if (_Stream.f_GetVersion() >= EProtocolVersion_AddAccessDenied)
			_Stream % m_bAccessDenied;
	}
}
