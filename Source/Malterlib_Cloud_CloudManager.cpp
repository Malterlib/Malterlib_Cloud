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
	}

	CCloudManager::~CCloudManager() = default;

	bool CCloudManager::CAppManagerInfo::operator == (CAppManagerInfo const &_Right) const
	{
		return f_Tuple() == _Right.f_Tuple();
	}

	bool CCloudManager::CApplicationKey::operator < (CApplicationKey const &_Right) const
	{
		return f_Tuple() < _Right.f_Tuple();
	}
}
