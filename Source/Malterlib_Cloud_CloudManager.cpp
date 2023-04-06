// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_CloudManager.h"

namespace NMib::NCloud
{
	CCloudManager::CCloudManager()
	{
		DMibPublishActorFunction(CCloudManager::f_RegisterAppManager);
		DMibPublishActorFunction(CCloudManager::f_EnumAppManagers);
		DMibPublishActorFunction(CCloudManager::f_EnumApplications);
		DMibPublishActorFunction(CCloudManager::f_RemoveAppManager);
		DMibPublishActorFunction(CCloudManager::f_RemoveSensor);
		DMibPublishActorFunction(CCloudManager::f_GetSensorReporter);
		DMibPublishActorFunction(CCloudManager::f_GetSensorReader);
		DMibPublishActorFunction(CCloudManager::f_GetLogReporter);
		DMibPublishActorFunction(CCloudManager::f_GetLogReader);
	}

	CCloudManager::~CCloudManager() = default;

	bool CCloudManager::CAppManagerDynamicInfo::f_HasErrors() const
	{
		return !m_bActive || !m_OtherErrors.f_IsEmpty();
	}

	uint32 CCloudManager::fs_ProtocolVersion_CloudManagerToAppManager(uint32 _CloudManagerVersion)
	{
		static_assert
			(
				CAppManagerInterface::EProtocolVersion_Current == CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications
				, "Add a new version mapping if streaming of m_ApplicationInfo changed"
			)
		;

		if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased4)
			return CAppManagerInterface::EProtocolVersion_ResumableUpdateNotifications;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased3)
			return CAppManagerInterface::EProtocolVersion_HostIDInApplicationInfo;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased2)
			return CAppManagerInterface::EProtocolVersion_AddLaunchInProcess;
		else if (_CloudManagerVersion >= ECloudManagerProtocolVersion_AppManagerVersionIncreased1)
			return CAppManagerInterface::EProtocolVersion_AddAutoUpdateAndExtendAppInfo;

		return CAppManagerInterface::EProtocolVersion_AddStatusSeverity;
	}
}
