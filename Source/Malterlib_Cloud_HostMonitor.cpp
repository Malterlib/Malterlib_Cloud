// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

namespace NMib::NCloud
{
	CHostMonitor::CHostMonitor(TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore, TCActor<CDistributedAppLogStoreLocal> const &_LogStore)
		: mp_pInternal(fg_Construct(this, _SensorStore, _LogStore))
	{
	}

	CHostMonitor::~CHostMonitor() = default;

	CHostMonitor::CInternal::CInternal(CHostMonitor *_pThis, TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore, TCActor<CDistributedAppLogStoreLocal> const &_LogStore)
		: m_SensorStore(_SensorStore)
		, m_LogStore(_LogStore)
		, m_pThis(_pThis)
	{
	}

	TCFuture<void> CHostMonitor::f_Init(EInitFlag _Flags, fp64 _HostMonitorInterval)
	{
		DMibFastCheck(!_HostMonitorInterval.f_IsInvalid());
		DMibFastCheck(_HostMonitorInterval == 0.0 || _HostMonitorInterval >= mc_MinimumHostMonitorInterval);

		auto &Internal = *mp_pInternal;

		Internal.m_FileActor = fg_Construct(fg_Construct(), "Host monitor file actor {}"_f << Internal.m_FileActorSequence++);
		Internal.m_Flags = _Flags;
		Internal.m_HostMonitorInterval = _HostMonitorInterval;

		fg_CallSafe(Internal, &CInternal::f_PeriodicUpdate) > fg_LogError("Malterlib/Cloud/HostMonitor", "Failed to run initial periodic update");

		co_return {};
	}

	TCFuture<void> CHostMonitor::CInternal::f_PeriodicUpdate()
	{
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (m_pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto SequenceSubscription = co_await m_UpdatePeriodicSequencer.f_Sequence();

		if (!m_UpdateTimerSubscription && m_HostMonitorInterval != 0.0)
		{
			m_UpdateTimerSubscription = co_await fg_RegisterTimer
				(
					m_HostMonitorInterval
					, [this]() -> TCFuture<void>
					{
						auto Result = co_await fg_CallSafe(*this, &CInternal::f_PeriodicUpdate).f_Wrap();
						if (!Result)
							DMibLogWithCategory(Malterlib/Cloud/HostMonitor, Error, "Failed to run periodic update");

						co_return {};
					}
				)
			;
		}

		co_await fg_CallSafe(*this, &CInternal::f_PeriodicUpdate_Diskspace, true);

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

		if (Internal.m_UpdateTimerSubscription)
			co_await fg_Exchange(Internal.m_UpdateTimerSubscription, nullptr)->f_Destroy();

		co_await Internal.m_UpdatePeriodicSequencer.f_Abort();

		TCActorResultVector<void> Destroys;

		for (auto &MonitoredPath : Internal.m_MonitoredPaths)
			MonitoredPath.f_Destroy() > Destroys.f_AddResult();

		co_await Destroys.f_GetResults();

		if (Internal.m_FileActor)
			co_await fg_Move(Internal.m_FileActor).f_Destroy();

		co_return {};
	}
}
