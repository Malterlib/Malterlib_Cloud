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

	static_assert(CVersionManager::EMinProtocolVersion <= 0x105);
}
