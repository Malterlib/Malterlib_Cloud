// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<void> CAppManagerActor::fp_InitHostMonitor()
	{
		mp_HostMonitor = fg_Construct(mp_SensorStore, mp_LogStore, mp_DatabaseActor);

		CHostMonitor::EInitFlag Flags = CHostMonitor::EInitFlag_None;

		if (mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("MonitorAllMounts", false).f_Boolean())
			Flags |= CHostMonitor::EInitFlag_MonitorAllMounts;

		co_await mp_HostMonitor(&CHostMonitor::f_Init, Flags, mp_HostMonitorInterval);

		CHostMonitor::CMonitorPathOptions PathOptions;
		PathOptions.m_Path = mp_State.m_RootDirectory;

		auto MonitorResult = co_await mp_HostMonitor(&CHostMonitor::f_MonitorPath, PathOptions).f_Wrap();
		if (MonitorResult)
			mp_MainDirectoryMonitorSubscription = fg_Move(*MonitorResult);
		else
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to monitor main directory: {}", MonitorResult.f_GetExceptionStr());

		co_return {};
	}
}
