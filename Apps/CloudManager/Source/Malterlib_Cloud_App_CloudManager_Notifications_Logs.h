// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer;

	struct CLogNotifications : public CAllowUnsafeThis
	{
		CLogNotifications(CCloudManagerServer &_This);

		TCFuture<void> f_Init();
		TCFuture<void> f_Destroy();

	private:

		struct CLogStatus
		{
			CDistributedAppLogReporter::CLogInfo const *f_GetInfo();

			CDistributedAppLogReporter::CLogInfo m_Info;
			bool m_bInfoValid = false;
		};

		struct CAlertConfig
		{
			CDistributedAppLogReader_LogEntrySubscriptionFilter m_Filter;
			TCOptional<CSlackActor::EPredefinedColor> m_SlackColor;
			TCOptional<CNotifications::EType> m_TypeFlags = CNotifications::EType_None;
			CStr m_Message = "```{}```";

			TCVector<CDistributedAppLogReader_LogKeyAndEntry> m_QueuedEntries;
		};

		void fp_ScheduleSlackNotification();
		TCFuture<void> fp_SendSlackNotifications();

		CCloudManagerServer &mp_This;

		TCVector<CAlertConfig> mp_AlertConfigs;

		CActorSubscription mp_LogSubscription;
		CActorSubscription mp_LogStatusSubscription;

		TCActorSequencerAsync<void> mp_UpdateSequencer;

		TCMap<CDistributedAppLogReporter::CLogInfoKey, CLogStatus> mp_LogStatuses;

		NTime::CTime mp_LastSeenTimestamp;

		bool mp_bSubscribedToLogs = false;
		bool mp_bSendingSlackNotifications = false;
		bool mp_bRescheduleSlackNotifications = false;
		bool mp_bSlackNotificationsScheduled = false;
	};
}
