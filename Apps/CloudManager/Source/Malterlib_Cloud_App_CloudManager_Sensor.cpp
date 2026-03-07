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
		co_await mp_AppSensorStore
			(
				&CDistributedAppSensorStoreLocal::f_StartWithDatabase
				, fg_TempCopy(mp_DatabaseActor)
				, mcp_DatabasePrefixSensor
				, g_ActorFunctor / [this](NDatabase::CDatabaseActor::CTransactionWrite _WriteTransaction) -> TCFuture<NDatabase::CDatabaseActor::CTransactionWrite>
				{
					co_return fg_Move((co_await fp_CleanupDatabase(fg_Move(_WriteTransaction), fg_Construct())).m_Transaction);
				}
			)
		;

		co_return {};
	}

	auto CCloudManagerServer::CDistributedAppSensorReporterImplementation::f_OpenSensorReporter(CSensorInfo _SensorInfo) -> TCFuture<CSensorReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

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
			co_return Auditor.f_AccessDenied("(Open sensor reporter)", Permissions);

		auto Key = SensorInfo.f_Key();

		auto Reporter = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_OpenSensorReporter, fg_Move(SensorInfo));

		Auditor.f_Info("Open sensor reporter ({})"_f << Key);

		co_return fg_Move(Reporter);
	}

	TCVector<CStr> CCloudManagerServer::fsp_SensorReadPermissions()
	{
		return {"CloudManager/ReadSensors", "CloudManager/ReadAll"};
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensors(CGetSensors _Params)
		-> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> Sensors;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensors", Permissions))
				co_return Auditor.f_AccessDenied("(Get sensors)", Permissions);

			Sensors = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensors, fg_Move(_Params));

			Auditor.f_Info("Get sensors");
		}

		co_return fg_Move(Sensors);
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensorReadings(CGetSensorReadings _Params)
		-> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> SensorReadings;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor readings", Permissions))
				co_return Auditor.f_AccessDenied("(Get sensor readings)", Permissions);

			SensorReadings = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensorReadings, fg_Move(_Params));

			Auditor.f_Info("Get sensor readings");
		}

		co_return fg_Move(SensorReadings);
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_GetSensorStatus(CGetSensorStatus _Params)
		-> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> SensorReadings;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get sensor status", Permissions))
				co_return Auditor.f_AccessDenied("(Get sensor status)", Permissions);

			SensorReadings = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_GetSensorStatus, fg_Move(_Params));

			Auditor.f_Info("Get sensor status");
		}

		co_return fg_Move(SensorReadings);
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_SubscribeSensors
		(
			TCVector<CDistributedAppSensorReader_SensorFilter> _Filters
			, TCActorFunctorWithID<TCFuture<void> (CSensorChange _Change)> _fOnChange
		)
		-> TCFuture<TCActorSubscriptionWithID<>>
	{
		TCActorSubscriptionWithID<> Subscription;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Subscribe sensors", Permissions))
				co_return Auditor.f_AccessDenied("(Subscribe sensors)", Permissions);

			Subscription = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SubscribeSensors, fg_Move(_Filters), fg_Move(_fOnChange));

			Auditor.f_Info("Subscribe sensors");
		}

		co_return fg_Move(Subscription);
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_SubscribeSensorReadings
		(
			TCVector<CDistributedAppSensorReader_SensorReadingSubscriptionFilter> _Filters
			, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading _Reading)> _fOnReading
		)
		-> TCFuture<TCActorSubscriptionWithID<>>
	{
		for (auto &Filter : _Filters)
		{
			if (Filter.m_Flags & CDistributedAppSensorReader_SensorReadingFilter::ESensorReadingsFlag_ReportNewestFirst)
				co_return DMibErrorInstance("ESensorReadingsFlag_ReportNewestFirst is not supported for subscriptions");
		}

		TCActorSubscriptionWithID<> Subscription;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Subscribe sensor readings", Permissions))
				co_return Auditor.f_AccessDenied("(Subscribe sensor readings)", Permissions);

			Subscription = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SubscribeSensorReadings, fg_Move(_Filters), fg_Move(_fOnReading));

			Auditor.f_Info("Subscribe sensor readings");
		}

		co_return fg_Move(Subscription);
	}

	auto CCloudManagerServer::CDistributedAppSensorReaderImplementation::f_SubscribeSensorStatus
		(
			TCVector<CDistributedAppSensorReader_SensorStatusFilter> _Filters
			, TCActorFunctorWithID<TCFuture<void> (CDistributedAppSensorReader_SensorKeyAndReading _Reading)> _fOnReading
		)
		-> TCFuture<TCActorSubscriptionWithID<>>
	{
		for (auto &Filter : _Filters)
		{
			if (Filter.m_Flags & CDistributedAppSensorReader_SensorReadingFilter::ESensorReadingsFlag_ReportNewestFirst)
				co_return DMibErrorInstance("ESensorReadingsFlag_ReportNewestFirst is not supported for subscriptions");
		}

		TCActorSubscriptionWithID<> Subscription;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_SensorReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Subscribe sensor status", Permissions))
				co_return Auditor.f_AccessDenied("(Subscribe sensor status)", Permissions);

			Subscription = co_await pThis->mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SubscribeSensorStatus, fg_Move(_Filters), fg_Move(_fOnReading));

			Auditor.f_Info("Subscribe sensor status");
		}

		co_return fg_Move(Subscription);
	}
}
