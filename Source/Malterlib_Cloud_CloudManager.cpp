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
}
