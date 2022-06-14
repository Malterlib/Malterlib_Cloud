// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Process/Platform>

namespace NMib::NCloud::NCloudManager
{
	constinit uint64 g_MaxDatabaseSize = constant_uint64(128) * 1024 * 1024 * 1024;

	CCloudManagerServer::CCloudManagerServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
#ifdef DPlatformFamily_macOS
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");

		CStr OriginalPath = Path;

		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;

		if (Path != OriginalPath)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Path);
#endif
	}

	CCloudManagerServer::~CCloudManagerServer()
	{
	}

	TCFuture<void> CCloudManagerServer::f_Init()
	{
		mp_DatabaseActor = fg_Construct(fg_Construct(), "Cloud manager database");
		auto MaxDatabaseSize = mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("MaxDatabaseSize", g_MaxDatabaseSize).f_Integer();
		co_await
			(
			 	mp_DatabaseActor
			 	(
				 	&CDatabaseActor::f_OpenDatabase
				 	, mp_AppState.m_RootDirectory / "CloudManagerDatabase"
				 	, MaxDatabaseSize
				)
			 	% "Faild to open database"
			)
		;
		auto Stats = co_await (mp_DatabaseActor(&CDatabaseActor::f_GetAggregateStatistics));
		auto TotalSizeUsed = Stats.f_GetTotalUsedSize();
		DMibLogWithCategory
			(
				CloudManager
				, Info
				, "Database uses {fe2}% of allotted space ({ns } / {ns } bytes)"
				, fp64(TotalSizeUsed) / fp64(MaxDatabaseSize) * 100.0
				, TotalSizeUsed
				, MaxDatabaseSize
			)
		;

		co_await (fp_SetupSensorStore() % "Failed to setup sensor store");
		co_await (fp_SetupLogStore() % "Failed to setup log store");

		co_await (fp_SetupPermissions() % "Failed to setup permissions");
		co_await (fp_SetupMonitor() % "Failed to setup monitor");
		co_await (fp_Publish() % "Failed to publish");

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_Destroy()
	{
		TCActorResultVector<void> Destroys;

		if (mp_MonitorTimerSubscription)
			mp_MonitorTimerSubscription->f_Destroy() > Destroys.f_AddResult();

		if (mp_CleanupTimerSubscription)
			mp_CleanupTimerSubscription->f_Destroy() > Destroys.f_AddResult();

		mp_ProtocolInterface.f_Destroy() > Destroys.f_AddResult();
		mp_SensorReporterInterface.f_Destroy() > Destroys.f_AddResult();
		mp_SensorReaderInterface.f_Destroy() > Destroys.f_AddResult();
		mp_LogReporterInterface.f_Destroy() > Destroys.f_AddResult();
		mp_LogReaderInterface.f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetResults();

		if (mp_AppSensorStore)
			co_await fg_Move(mp_AppSensorStore).f_Destroy();

		if (mp_AppLogStore)
			co_await fg_Move(mp_AppLogStore).f_Destroy();

		if (mp_DatabaseActor)
			co_await fg_Move(mp_DatabaseActor).f_Destroy();

		co_return {};
	}
}
