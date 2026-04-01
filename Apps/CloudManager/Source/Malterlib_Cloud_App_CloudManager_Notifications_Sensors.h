// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer;

	struct CSensorNotifications : public CAllowUnsafeThis
	{
		CSensorNotifications(CCloudManagerServer &_This);

		TCFuture<void> f_Init();
		TCFuture<void> f_Destroy();
		TCFuture<void> f_UpdatePeriodicSensorNotifications(bool _bForceAtOnce);

	private:
		struct CSensorStatus
		{
			CDistributedAppSensorReporter::CSensorInfo const *f_GetInfo();

			CDistributedAppSensorReporter::CSensorInfo m_Info;
			CDistributedAppSensorReporter::CSensorReading m_LastReading;

			NCloudManagerDatabase::CSensorNotificationStateValue m_State;

			CStopwatch m_ProblemStopwatch;

			CDistributedAppSensorReporter::EStatusSeverity m_LastCombinedStatus;

			bool m_bInfoValid = false;
			bool m_bLastCombinedStatusValid = false;
		};

		void fp_SchedulePeriodicSensorNotificationsOutOfBand(bool _bForceAtOnce);
		TCFuture<void> fp_UpdateSensorFromReading(CDistributedAppSensorReporter::CSensorInfoKey _SensorKey, CTime _Now);

		CCloudManagerServer &mp_This;
		CActorSubscription mp_SensorSubscription;
		CActorSubscription mp_SensorStatusSubscription;

		CSequencer mp_UpdateSequencer{"SensorNotifications UpdateSequencer"};

		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CSensorStatus> mp_SensorStatuses;
		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> mp_RemovedSensors;

		CSlackActor::CMessage mp_LastSentProblemMessage;
		CNotifications::EType mp_ProblemNotificationFlags = CNotifications::EType_None;

		fp64 mp_SensorAlertThreshold = 2.0 * 60.0;

		bool mp_bSubscribedToSensors = false;
		bool mp_bSensorsUpdating = false;
		bool mp_bSensorsReschedule = false;
		bool mp_bOutOfBandSensorsUpdateScheduled = false;
	};
}
