// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NContainer;
	using namespace NStr;
	using namespace NException;
	using namespace NFile;

	struct CHostMonitor::CInternal : public CActorInternal
	{
		enum ESensorType
		{
			ESensorType_Free
			, ESensorType_FreePercent
			, ESensorType_Total
		};

		struct CMonitoredPath
		{
			TCFuture<void> f_Destroy();

			CMonitorPathOptions m_Options;
			mint m_Refcount = 0;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_FreeReporter;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_FreePercentReporter;
			TCOptional<CDistributedAppSensorReporter::CSensorReporter> m_TotalReporter;
		};

		struct CAutomaticMount
		{
			CActorSubscription m_MonitorPathSubscription;
		};

		TCFuture<void> f_PeriodicUpdate();
		TCFuture<void> f_PeriodicUpdate_Diskspace(bool _bCanSkip);
		TCFuture<void> f_PeriodicUpdate_Diskspace_UpdateMounts();

		CInternal(CHostMonitor *_pThis, TCActor<CDistributedAppSensorStoreLocal> const &_SensorStore, TCActor<CDistributedAppLogStoreLocal> const &_LogStore);

		CHostMonitor *m_pThis;

		TCActor<CDistributedAppSensorStoreLocal> m_SensorStore;
		TCActor<CDistributedAppLogStoreLocal> m_LogStore;
		TCActor<CSeparateThreadActor> m_FileActor;
		CActorSubscription m_UpdateTimerSubscription;
		TCMap<CStr, CMonitoredPath> m_MonitoredPaths;
		TCMap<CStr, CAutomaticMount> m_AutomaticMounts;
		TCActorSequencerAsync<void> m_UpdatePeriodicSequencer;
		TCActorSequencerAsync<void> m_UpdatePeriodicDiskSpaceSequencer;
		TCVector<TCPromise<void>> m_UpdatePeriodicWaitList;
		fp64 m_HostMonitorInterval = 60.0;
		mint m_FileActorSequence = 0;
		EInitFlag m_Flags = EInitFlag_None;
	};
}
