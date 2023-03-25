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
}
