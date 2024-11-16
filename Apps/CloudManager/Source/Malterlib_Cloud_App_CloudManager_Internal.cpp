// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager.h"

#include <Mib/Web/WebSocket>
#include <Mib/Network/SSL>
#include <Mib/Network/Sockets/SSL>
#include <Mib/Process/Platform>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCloudManager
{
	constexpr CStr const CCloudManagerServer::mc_DatabasePrefixLog = gc_Str<"mib.llog">; // llog = local log
	constexpr CStr const CCloudManagerServer::mc_DatabasePrefixSensor = gc_Str<"mib.lsensor">; // lsensor = local sensor

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
		co_await (fp_SetupDatabase() % "Failed to setup database");

		co_await (fp_SetupSensorStore() % "Failed to setup sensor store");
		co_await (fp_SetupLogStore() % "Failed to setup log store");

		co_await (fp_SetupPermissions() % "Failed to setup permissions");
		co_await (fp_SetupMonitor() % "Failed to setup monitor");

		co_await (fp_SetupCleanup() % "Failed to setup cleanup");
		co_await (mp_Notifications.f_Init() % "Failed to setup notifications");

		// Publish last so notifications are not missed
		co_await (fp_Publish() % "Failed to publish");

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_Destroy()
	{
		NConcurrency::CLogError LogError("CloudManager");

		co_await mp_LogNotifications.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy log notifications");;
		co_await mp_SensorNotifications.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy sensor notifications");;
		co_await mp_UpdateNotifications.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy update notifications");;
		co_await mp_Notifications.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy notifications");;

		{
			TCFutureVector<void> Destroys;

			if (mp_MonitorTimerSubscription)
				mp_MonitorTimerSubscription->f_Destroy() > Destroys;

			if (mp_CleanupTimerSubscription)
				mp_CleanupTimerSubscription->f_Destroy() > Destroys;

			for (auto &Subscription : mp_ExpectedOsVersionSubscriptions)
				fg_Move(Subscription.m_fVersionRangeChanged).f_Destroy() > Destroys;

			mp_ExpectedOsVersionSubscriptions.f_Clear();

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy remote subscriptions");
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &Subscription : mp_ExpectedOsVersionSubscriptions)
			{
				if (Subscription.m_fVersionRangeChanged)
					fg_Move(Subscription.m_fVersionRangeChanged).f_Destroy() > Destroys;
			}
			mp_ExpectedOsVersionSubscriptions.f_Clear();

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy expected OS version subscriptions");
		}

		{
			TCFutureVector<void> Destroys;

			for (auto &AppManager : mp_AppManagers)
				AppManager.f_Destroy(*this) > Destroys;

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy app manager subscriptions");
		}

		{
			TCFutureVector<void> Destroys;

			mp_ProtocolInterface.f_Destroy() > Destroys;
			mp_SensorReporterInterface.f_Destroy() > Destroys;
			mp_SensorReaderInterface.f_Destroy() > Destroys;
			mp_LogReporterInterface.f_Destroy() > Destroys;
			mp_LogReaderInterface.f_Destroy() > Destroys;
			mp_AppManagerCloudManagerInterface.f_Destroy() > Destroys;

			co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy interfaces");
		}

		if (mp_AppSensorStore)
			co_await fg_Move(mp_AppSensorStore).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app sensor store");;

		if (mp_AppLogStore)
			co_await fg_Move(mp_AppLogStore).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy app log store");;

		if (mp_DatabaseActor)
			co_await fg_Move(mp_DatabaseActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy database actor");;

		co_return {};
	}
}
