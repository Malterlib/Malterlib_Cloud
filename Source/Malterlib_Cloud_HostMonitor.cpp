// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	CHostMonitor::CHostMonitor
		(
			TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore
			, TCActor<CDistributedAppLogStoreLocal> const &_LogStore
			, TCActor<NDatabase::CDatabaseActor> const &_Database
		)
		: mp_pInternal(fg_Construct(this, _SensorStore, _LogStore, _Database))
	{
	}

	CHostMonitor::~CHostMonitor() = default;

	CHostMonitor::CInternal::CInternal
		(
			CHostMonitor *_pThis
			, TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore
			, TCActor<CDistributedAppLogStoreLocal> const &_LogStore
			, TCActor<CDatabaseActor> const &_Database
		)
		: m_SensorStore(_SensorStore)
		, m_LogStore(_LogStore)
		, m_Database(_Database)
		, m_pThis(_pThis)
	{
	}

	auto CHostMonitor::f_Init(CConfig &&_Config) -> TCFuture<CInitResult>
	{
		DMibFastCheck(!_Config.m_Interval.f_IsInvalid());
		DMibFastCheck(_Config.m_Interval == 0.0 || _Config.m_Interval >= mc_MinimumHostMonitorInterval);

		DMibFastCheck(!_Config.m_PatchInterval.f_IsInvalid());
		DMibFastCheck(_Config.m_PatchInterval == 0.0 || _Config.m_PatchInterval >= mc_MinimumHostMonitorPatchInterval);

		if (_Config.m_PatchInterval != 0.0 && _Config.m_PatchInterval < _Config.m_Interval)
			co_return DMibErrorInstance("Patch interval cannot be lower than interval");

		auto &Internal = *mp_pInternal;

		Internal.m_FileActor = fg_Construct(fg_Construct(), "Host monitor file actor {}"_f << Internal.m_FileActorSequence++);
		Internal.m_Config = fg_Move(_Config);

		co_await Internal.f_SetupDatabase();

		Internal.m_CurrentOsVersion = co_await Internal.f_GetOsNameAndVersion();

		CInitResult Result;
		Result.m_OsName = Internal.m_CurrentOsVersion.m_Identifier;

		if (Internal.m_Config.m_Interval != 0.0 || Internal.m_Config.m_PatchInterval != 0.0)
		{
			Internal.m_UpdateTimerSubscription = co_await fg_RegisterTimer
				(
					Internal.m_Config.m_Interval ? Internal.m_Config.m_Interval : 60.0
					, [this]() -> TCFuture<void>
					{
						auto &Internal = *mp_pInternal;

						auto Result = co_await fg_CallSafe(Internal, &CInternal::f_PeriodicUpdate).f_Wrap();
						if (!Result)
							DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed to run periodic update");

						co_return {};
					}
				)
			;
		}

		if (Internal.m_Config.m_PatchInterval != 0.0)
			Internal.f_PeriodicUpdate_Patch(true) > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to run initial patch update");

		co_return fg_Move(Result);
	}

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate()
	{
		auto OnResume = co_await m_pThis->f_CheckDestroyedOnResume();

		auto SequenceSubscription = co_await m_UpdatePeriodicSequencer.f_Sequence();

		co_await
			(
				f_PeriodicUpdate_Diskspace(true)
				+ f_PeriodicUpdate_Patch(true)
			)
		;

		co_return {};
	}

	TCFuture<void> CHostMonitor::CInternal::CMonitoredPath::f_Destroy()
	{
		co_await ECoroutineFlag_AllowReferences; // Nothing accessed after suspend

		TCActorResultVector<void> Destroys;

		if (m_FreeReporter)
		{
			fg_Move(m_FreeReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();
			m_FreeReporter.f_Clear();
		}

		if (m_FreePercentReporter)
		{
			fg_Move(m_FreePercentReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();
			m_FreePercentReporter.f_Clear();
		}

		if (m_TotalReporter)
		{
			fg_Move(m_TotalReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();
			m_TotalReporter.f_Clear();
		}

		co_await (co_await Destroys.f_GetResults() | g_Unwrap);

		co_return {};
	}

	TCFuture<void> CHostMonitor::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		CLogError LogError("Malterlib/Cloud/HostMonitor");

		if (Internal.m_UpdateTimerSubscription)
			co_await fg_Exchange(Internal.m_UpdateTimerSubscription, nullptr)->f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy update timer subscription");

		co_await fg_Move(Internal.m_UpdatePeriodicSequencer).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy update periodic sequencer");

		TCActorResultVector<void> Destroys;

		for (auto &MonitoredPath : Internal.m_MonitoredPaths)
			MonitoredPath.f_Destroy() > Destroys.f_AddResult();

		if (Internal.m_OsVersionReporter && Internal.m_OsVersionReporter->m_fReportReadings)
			fg_Move(Internal.m_OsVersionReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();

		if (Internal.m_OsVersionStatusReporter && Internal.m_OsVersionStatusReporter->m_fReportReadings)
			fg_Move(Internal.m_OsVersionStatusReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();

		if (Internal.m_OsPatchStatusReporter && Internal.m_OsPatchStatusReporter->m_fReportReadings)
			fg_Move(Internal.m_OsPatchStatusReporter->m_fReportReadings).f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetUnwrappedResults().f_Wrap() > LogError.f_Warning("Failed to destroy host monitor");

		if (Internal.m_FileActor)
			co_await fg_Move(Internal.m_FileActor).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy file actor");;

		co_return {};
	}
}
