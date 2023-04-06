// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer;

	struct CSensorNotifications : public CAllowUnsafeThis
	{
		CSensorNotifications(CCloudManagerServer &_This);

		TCFuture<void> f_Init();
		TCFuture<void> f_Destroy();
		TCFuture<void> f_UpdatePeriodicSensorNotifications();

	private:
		void fp_SchedulePeriodicSensorNotificationsOutOfBand();
		TCFuture<void> fp_UpdateSensorFromReading(CDistributedAppSensorReporter::CSensorInfoKey _SensorKey, CTime _Now);

		struct CSensorStatus
		{
			CDistributedAppSensorReporter::CSensorInfo const *f_GetInfo();

			CDistributedAppSensorReporter::CSensorInfo m_Info;
			CDistributedAppSensorReporter::CSensorReading m_LastReading;

			NCloudManagerDatabase::CSensorNotificationStateValue m_State;

			CClock m_ProblemClock;

			CDistributedAppSensorReporter::EStatusSeverity m_LastCombinedStatus;

			bool m_bInfoValid = false;
			bool m_bLastCombinedStatusValid = false;
		};

		CCloudManagerServer &mp_This;
		CActorSubscription mp_SensorSubscription;
		CActorSubscription mp_SensorStatusSubscription;

		TCActorSequencerAsync<void> mp_UpdateSequencer;

		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, CSensorStatus> mp_SensorStatuses;
		TCSet<CDistributedAppSensorReporter::CSensorInfoKey> mp_RemovedSensors;

		CSlackActor::CMessage mp_LastSentProblemMessage;

		fp64 mp_SensorAlertThreshold = 2.0 * 60.0;

		bool mp_bSubscribedToSensors = false;
		bool mp_bSensorsUpdating = false;
		bool mp_bSensorsReschedule = false;
		bool mp_bOutOfBandSensorsUpdateScheduled = false;
	};
}
