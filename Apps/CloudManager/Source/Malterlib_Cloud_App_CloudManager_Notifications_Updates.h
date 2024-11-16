// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NCloudManager
{
	struct CCloudManagerServer;

	struct CUpdateNotifications : public CAllowUnsafeThis
	{
		CUpdateNotifications(CCloudManagerServer &_This);

		TCFuture<void> f_Init();
		TCFuture<void> f_Destroy();
		TCFuture<void> f_ProcessApplicationUpdateNotification(CStr _AppManagerID, CAppManagerInterface::CUpdateNotification _Notification);

	private:
		TCFuture<TCMap<CStr, CStr>> fp_ReportApplicationUpdateToSlack(CStr _AppManagerID, NCloudManagerDatabase::CApplicationUpdateStateValue _UpdateState);

		void fp_ScheduleDeferredNotifications();
		TCFuture<void> fp_SendDeferredNotifications();

		struct CDeferredUpdates
		{
			struct CUpdate
			{
				NCloudManagerDatabase::CApplicationUpdateStateValue m_LastUpdateState;
				DLinkDS_Link(CUpdate, m_Link);
				CStr m_AppManagerID;
			};

			struct CAppManager
			{
				TCMap<CStr, CUpdate> m_Updates;
			};

			TCMap<CStr, CAppManager> m_AppManagers;
			DLinkDS_List(CUpdate, m_Link) m_OrderedUpdates;
		};

		CCloudManagerServer &mp_This;

		CSequencer mp_ProcessSequencer{"UpdateNotifications ProcessSequencer"};

		CDeferredUpdates mp_DeferredUpdates;

		fp64 mp_UpdateStageNotificationThreshold = 0.1;
		fp64 mp_UpdateLongRunningThreshold = 2.0 * 60.0;
		bool mp_bScheduledDeferredNotifications = false;
	};
}

