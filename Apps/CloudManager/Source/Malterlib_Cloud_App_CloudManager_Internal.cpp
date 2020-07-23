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
	CCloudManagerServer::CCloudManagerServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
#ifdef DPlatformFamily_OSX
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/usr/local/bin") < 0)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", "/usr/local/bin:" + Path);
#endif
	}

	CCloudManagerServer::~CCloudManagerServer()
	{
	}

	TCFuture<void> CCloudManagerServer::f_Init()
	{
		mp_DatabaseActor = fg_Construct(fg_Construct(), "Cloud manager database");
		co_await
			(
			 	mp_DatabaseActor
			 	(
				 	&CDatabaseActor::f_OpenDatabase
				 	, mp_AppState.m_RootDirectory / "CloudManagerDatabase"
				 	, constant_uint64(128) * 1024 * 1024 * 1024
				)
			 	% "Faild to open database"
			)
		;
		co_await (fp_SetupSensorStore() % "Failed to setup sensor store");

		co_await (fp_SetupPermissions() % "Failed to setup permissions");
		co_await (fp_SetupMonitor() % "Failed to setup monitor");
		co_await (fp_Publish() % "Failed to publish");

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_Destroy()
	{
		TCActorResultVector<void> Destroys;

		mp_ProtocolInterface.f_Destroy() > Destroys.f_AddResult();
		mp_SensorReporterInterface.f_Destroy() > Destroys.f_AddResult();
		mp_SensorReaderInterface.f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetResults();

		if (mp_AppSensorStore)
			co_await fg_Move(mp_AppSensorStore).f_Destroy();

		if (mp_DatabaseActor)
			co_await fg_Move(mp_DatabaseActor).f_Destroy();

		co_return {};
	}
}
