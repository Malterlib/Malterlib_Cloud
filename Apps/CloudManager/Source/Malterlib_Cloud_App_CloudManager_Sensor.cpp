// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/CommandLine/AnsiEncoding>

namespace NMib::NCloud::NCloudManager
{
	TCFuture<void> CCloudManagerServer::fp_SetupSensorStore()
	{
		mp_AppSensorStore = fg_Construct(mp_AppState.m_DistributionManager, mp_AppState.m_TrustManager);
		co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_StartWithDatabase, fg_TempCopy(mp_DatabaseActor), "mib.lsensor");

		co_return {};
	}

	auto CCloudManagerServer::CDistributedAppSensorReporterImplementation::f_OpenSensorReporter(CSensorInfo &&_SensorInfo) -> TCFuture<CSensorReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = g_OnResume / [pThis]
			{
				if (pThis->f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto &HostInfo = Auditor.f_HostInfo();
		auto ReportingHostID = HostInfo.f_GetRealHostID();

		NContainer::TCVector<CStr> Permissions;

		CDistributedAppSensorReporter::CSensorInfo SensorInfo = fg_Move(_SensorInfo);
		if (!SensorInfo.m_HostID)
		{
			SensorInfo.m_HostID = ReportingHostID;
			SensorInfo.m_HostName = HostInfo.f_GetHostInfo().m_FriendlyName;
		}

		if (SensorInfo.m_HostID == ReportingHostID)
			Permissions.f_Insert("CloudManager/ReportSensorReadings");
		else
		{
			Permissions.f_Insert("CloudManager/ReportSensorReadingsOnBehalfOf/All");
			Permissions.f_Insert("CloudManager/ReportSensorReadingsOnBehalfOf/{}"_f << SensorInfo.m_HostID);
		}

		if (!co_await pThis->mp_Permissions.f_HasPermission("Open sensor reporter", Permissions, Auditor.f_HostInfo()))
			co_return Auditor.f_AccessDenied("(Open sensor reporter)");

		auto Key = SensorInfo.f_Key();

		auto Reporter = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));

		Auditor.f_Info("Open sensor reporter ({})"_f << Key);

		co_return fg_Move(Reporter);
	}

	TCVector<CStr> CCloudManagerServer::fsp_SensorReadPermissions()
	{
		return {"CloudManager/ReadSensors", "CloudManager/ReadAll"};
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensors(CDistributedAppSensorReader_SensorFilter &&_Filter, uint32 _BatchSize)
		-> TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> Sensors;
		{
			auto pThis = m_pThis;
			auto OnResume = g_OnResume / [pThis]
				{
					if (pThis->f_IsDestroyed())
						DMibError("Shutting down");
				}
			;

			auto Auditor = pThis->mp_AppState.f_Auditor();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensors", fsp_SensorReadPermissions()))
				co_return Auditor.f_AccessDenied("(Get sensors)");

			Sensors = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensors, fg_Move(_Filter), _BatchSize);

			Auditor.f_Info("Get sensors");
		}

		for (auto iSensor = co_await fg_Move(Sensors).f_GetIterator(); iSensor; co_await ++iSensor)
			co_yield *iSensor;

		co_return {};
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensorReadings(CDistributedAppSensorReader_SensorReadingFilter &&_Filter, uint32 _BatchSize)
		-> TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> SensorReadings;
		{
			auto pThis = m_pThis;
			auto OnResume = g_OnResume / [pThis]
				{
					if (pThis->f_IsDestroyed())
						DMibError("Shutting down");
				}
			;

			auto Auditor = pThis->mp_AppState.f_Auditor();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor readings", fsp_SensorReadPermissions()))
				co_return Auditor.f_AccessDenied("(Get sensor readings)");

			SensorReadings = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensorReadings, fg_Move(_Filter), _BatchSize);

			Auditor.f_Info("Get sensor readings");
		}

		for (auto iReading = co_await fg_Move(SensorReadings).f_GetIterator(); iReading; co_await ++iReading)
			co_yield *iReading;

		co_return {};
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensorStatus(CDistributedAppSensorReader_SensorFilter &&_Filter, uint32 _BatchSize)
		-> TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> SensorReadings;
		{
			auto pThis = m_pThis;
			auto OnResume = g_OnResume / [pThis]
				{
					if (pThis->f_IsDestroyed())
						DMibError("Shutting down");
				}
			;

			auto Auditor = pThis->mp_AppState.f_Auditor();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor status", fsp_SensorReadPermissions()))
				co_return Auditor.f_AccessDenied("(Get sensor status)");

			SensorReadings = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensorStatus, fg_Move(_Filter), _BatchSize);

			Auditor.f_Info("Get sensor status");
		}

		for (auto iReading = co_await fg_Move(SensorReadings).f_GetIterator(); iReading; co_await ++iReading)
			co_yield *iReading;

		co_return {};
	}
}
