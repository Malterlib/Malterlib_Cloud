// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_AppManager.h"
#include <Mib/Encoding/ToJson>

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
		DMibPublishActorFunction(CAppManagerInterface::f_SubscribeUpdateNotifications);
		DMibPublishActorFunction(CAppManagerInterface::f_SubscribeChangeNotifications);
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
	DMibDistributedStreamImplement(CAppManagerInterface::CUpdateNotification);
	
	CAppManagerInterface::~CAppManagerInterface() = default;

	static_assert(CVersionManager::EProtocolVersion_Min <= 0x105);

	NStr::CStr CAppManagerInterface::fs_UpdateStageToStr(EUpdateStage _Stage)
	{
		switch (_Stage)
		{
		case EUpdateStage_Failed: return NStr::gc_Str<"failed">;
		case EUpdateStage_None: return NStr::gc_Str<"none">;
		case EUpdateStage_SyncStart: return NStr::gc_Str<"sync start">;
		case EUpdateStage_ChangeEncryption: return NStr::gc_Str<"change encryption">;
		case EUpdateStage_DownloadVersion: return NStr::gc_Str<"download version">;
		case EUpdateStage_Unpack: return NStr::gc_Str<"unpack">;
		case EUpdateStage_StopOldApp: return NStr::gc_Str<"stop old app">;
		case EUpdateStage_PreUpdateScript: return NStr::gc_Str<"pre update script">;
		case EUpdateStage_UpdateApplicationFiles: return NStr::gc_Str<"update application files">;
		case EUpdateStage_SaveApplicationState: return NStr::gc_Str<"save application state">;
		case EUpdateStage_PostUpdateScript: return NStr::gc_Str<"post update script">;
		case EUpdateStage_StartNewApp: return NStr::gc_Str<"start new app">;
		case EUpdateStage_PostLaunch: return NStr::gc_Str<"post launch">;
		case EUpdateStage_Finished: return NStr::gc_Str<"finished">;
		}

		DMibNeverGetHere;
		return NStr::gc_Str<"unknown">;
	}

	bool CAppManagerInterface::CUpdateNotification::f_IsDone() const
	{
		return m_Stage == EUpdateStage_Failed || m_Stage == EUpdateStage_Finished;
	}

	NEncoding::CEJSONSorted CAppManagerInterface::CUpdateNotification::f_ToJson() const
	{
		using namespace NEncoding;

		CEJSONSorted Return;
		Return["UniqueSequence"] = fg_ToJson(m_UniqueSequence);
		Return["UpdateID"] = fg_ToJson(m_UpdateID);
		Return["Application"] = fg_ToJson(m_Application);
		Return["Message"] = fg_ToJson(m_Message);
		Return["VersionID"] = fg_ToJson(m_VersionID);
		Return["VersionTime"] = fg_ToJson(m_VersionTime);
		Return["StartUpdateTime"] = fg_ToJson(m_StartUpdateTime);
		Return["Stage"] = fg_ToJson(m_Stage);
		Return["UpdateTime"] = fg_ToJson(m_UpdateTime);
		Return["bCoordinateWait"] = fg_ToJson(m_bCoordinateWait);
		return Return;
	}

	NEncoding::CEJSONSorted CAppManagerInterface::CApplicationInfo::f_ToJson() const
	{
		using namespace NEncoding;

		CEJSONSorted Return;
		Return["Status"] = fg_ToJson(m_Status);
		Return["StatusSeverity"] = fg_ToJson(m_StatusSeverity);
		Return["EncryptionStorage"] = fg_ToJson(m_EncryptionStorage);
		Return["EncryptionFileSystem"] = fg_ToJson(m_EncryptionFileSystem);
		Return["ParentApplication"] = fg_ToJson(m_ParentApplication);
		Return["HostID"] = fg_ToJson(m_HostID);
		Return["Version"] = fg_ToJson(m_Version);
		Return["VersionInfo"] = fg_ToJson(m_VersionInfo);
		Return["FailedVersion"] = fg_ToJson(m_FailedVersion);
		Return["FailedVersionInfo"] = fg_ToJson(m_FailedVersionInfo);
		Return["FailedVersionError"] = fg_ToJson(m_FailedVersionError);
		Return["WantVersion"] = fg_ToJson(m_WantVersion);
		Return["WantVersionInfo"] = fg_ToJson(m_WantVersionInfo);
		Return["NewestUnconditionalVersion"] = fg_ToJson(m_NewestUnconditionalVersion);
		Return["NewestUnconditionalVersionInfo"] = fg_ToJson(m_NewestUnconditionalVersionInfo);
		Return["VersionManagerApplication"] = fg_ToJson(m_VersionManagerApplication);
		Return["UpdateGroup"] = fg_ToJson(m_UpdateGroup);
		Return["Executable"] = fg_ToJson(m_Executable);
		Return["Parameters"] = fg_ToJson(m_Parameters);
		Return["RunAsUser"] = fg_ToJson(m_RunAsUser);
		Return["RunAsGroup"] = fg_ToJson(m_RunAsGroup);
		Return["bRunAsUserHasShell"] = fg_ToJson(m_bRunAsUserHasShell);
		Return["Backup_IncludeWildcards"] = fg_ToJson(m_Backup_IncludeWildcards);
		Return["Backup_ExcludeWildcards"] = fg_ToJson(m_Backup_ExcludeWildcards);
		Return["Backup_AddSyncFlagsWildcards"] = fg_ToJson(m_Backup_AddSyncFlagsWildcards);
		Return["Backup_RemoveSyncFlagsWildcards"] = fg_ToJson(m_Backup_RemoveSyncFlagsWildcards);
		Return["Backup_NewBackupInterval"] = fg_ToJson(m_Backup_NewBackupInterval);
		Return["bAutoUpdate"] = fg_ToJson(m_bAutoUpdate);
		Return["UpdateTags"] = fg_ToJson(m_UpdateTags);
		Return["UpdateBranches"] = fg_ToJson(m_UpdateBranches);
		Return["UpdateScriptPreUpdate"] = fg_ToJson(m_UpdateScriptPreUpdate);
		Return["UpdateScriptPostUpdate"] = fg_ToJson(m_UpdateScriptPostUpdate);
		Return["UpdateScriptPostLaunch"] = fg_ToJson(m_UpdateScriptPostLaunch);
		Return["UpdateScriptOnError"] = fg_ToJson(m_UpdateScriptOnError);
		Return["Dependencies"] = fg_ToJson(m_Dependencies);
		Return["bSelfUpdateSource"] = fg_ToJson(m_bSelfUpdateSource);
		Return["bDistributedApp"] = fg_ToJson(m_bDistributedApp);
		Return["bStopOnDependencyFailure"] = fg_ToJson(m_bStopOnDependencyFailure);
		Return["bBackupEnabled"] = fg_ToJson(m_bBackupEnabled);
		Return["bLaunchInProcess"] = fg_ToJson(m_bLaunchInProcess);
		return Return;
	}
}
