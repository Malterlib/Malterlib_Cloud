// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	CUpdateNotifications::CUpdateNotifications(CCloudManagerServer &_This)
		: mp_This(_This)
	{
	}

	TCFuture<void> CUpdateNotifications::f_Init()
	{
		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		mp_UpdateStageNotificationThreshold = fg_Const(mp_This.mp_AppState.m_ConfigDatabase.m_Data)
			.f_GetMemberValue("UpdateStageNotificationThreshold", mp_UpdateStageNotificationThreshold)
			.f_Float()
		;
		mp_UpdateLongRunningThreshold = fg_Const(mp_This.mp_AppState.m_ConfigDatabase.m_Data).f_GetMemberValue("UpdateLongRunningThreshold", mp_UpdateLongRunningThreshold).f_Float();

		{
			auto WriteResult = co_await mp_This.mp_DatabaseActor
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [this]
					(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) mutable -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						co_await ECoroutineFlag_CaptureMalterlibExceptions;

						auto WriteTransaction = fg_Move(_Transaction);
						if (_bCompacting)
							WriteTransaction = co_await mp_This.self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

						bool bHaveDeferred = false;
						for (auto iState = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationUpdateStateKey::mc_Prefix); iState;)
						{
							auto Key = iState.f_Key<CApplicationUpdateStateKey>();
							auto Value = iState.f_Value<CApplicationUpdateStateValue>();

							if (!Value.m_bDeferred)
							{
								if (Value.m_LastNotification.f_IsDone())
									iState.f_Delete();
								else
									++iState;

								continue;
							}

							bHaveDeferred = true;
							auto Mapping = mp_DeferredUpdates.m_AppManagers[Key.m_AppManagerHostID].m_Updates(Value.m_LastUpdateID);
							auto &Update = *Mapping;
							if (Mapping.f_WasCreated())
								mp_DeferredUpdates.m_OrderedUpdates.f_Insert(Update);

							Update.m_AppManagerID = Key.m_AppManagerHostID;
							Update.m_LastUpdateState = fg_Move(Value);

							++iState;
						}

						if (bHaveDeferred)
							fp_ScheduleDeferredNotifications();

						co_return fg_Move(WriteTransaction);
					}
				)
				.f_Wrap()
			;

			if (!WriteResult)
			{
				DMibLogWithCategory(CloudManager, Critical, "Error updating update notification state in database: {}", WriteResult.f_GetExceptionStr());
				co_return WriteResult.f_GetException();
			}
		}

		co_return {};
	}

	TCFuture<void> CUpdateNotifications::f_Destroy()
	{
		co_await fg_Move(mp_ProcessSequencer).f_Destroy();

		co_return {};
	}

	void CUpdateNotifications::fp_ScheduleDeferredNotifications()
	{
		if (mp_bScheduledDeferredNotifications)
			return;

		mp_bScheduledDeferredNotifications = true;

		fg_Timeout(1.0) > [this]
			{
				mp_bScheduledDeferredNotifications = false;
				fp_SendDeferredNotifications() > fg_LogError("CloudManager", "Error sending deferred sensor notifications");
			}
		;
	}

	TCFuture<void> CUpdateNotifications::fp_SendDeferredNotifications()
	{
		auto SequenceSubscription = co_await mp_ProcessSequencer.f_Sequence();

		auto DeferredUpdates = fg_Move(mp_DeferredUpdates);

		for (auto &Update : DeferredUpdates.m_OrderedUpdates)
			co_await fg_CallSafe(*this, &CUpdateNotifications::fp_ReportApplicationUpdateToSlack, Update.m_AppManagerID, Update.m_LastUpdateState);

		co_return {};
	}

	TCFuture<void> CUpdateNotifications::f_ProcessApplicationUpdateNotification(CStr _AppManagerID, CAppManagerInterface::CUpdateNotification _Notification)
	{
		if (_Notification.m_bCoordinateWait)
			co_return {};

		auto pAppManager = mp_This.mp_AppManagers.f_FindEqual(_AppManagerID);

		if (!pAppManager)
			co_return {};

		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		auto SequenceSubscription = co_await mp_ProcessSequencer.f_Sequence();

		{
			auto &AppManager = mp_DeferredUpdates.m_AppManagers[_AppManagerID];
			if (!AppManager.m_Updates.f_FindEqual(_Notification.m_UpdateID) && !AppManager.m_Updates.f_IsEmpty())
			{
				// Flush any old updates out
				for (auto &Update : AppManager.m_Updates)
				{
					co_await fg_CallSafe(*this, &CUpdateNotifications::fp_ReportApplicationUpdateToSlack, _AppManagerID, Update.m_LastUpdateState).f_Wrap()
						> fg_LogError("CloudManager", "Error sending deferred sensor notification")
					;
				}

				AppManager.m_Updates.f_Clear();
			}
		}

		TCPromise<CApplicationUpdateStateValue> StateValuePromise;
		auto StateValueFuture = StateValuePromise.f_Future();

		{
			auto WriteResult = co_await mp_This.mp_DatabaseActor
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [this, Notification = _Notification, _AppManagerID, StateValuePromise = fg_Move(StateValuePromise)]
					(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						co_await ECoroutineFlag_CaptureMalterlibExceptions;

						auto WriteTransaction = fg_Move(_Transaction);
						if (_bCompacting)
							WriteTransaction = co_await mp_This.self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

						auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();

						CApplicationUpdateStateKey Key;
						Key.m_AppManagerHostID = _AppManagerID;
						Key.m_Application = Notification.m_Application;

						CApplicationUpdateStateValue Value;
						if (WriteCursor.f_FindEqual(Key))
							Value = WriteCursor.f_Value<CApplicationUpdateStateValue>();

						if (Value.m_LastUpdateID != Notification.m_UpdateID)
						{
							Value.m_LastUpdateID = Notification.m_UpdateID;
							Value.m_SlackTimestamps.f_Clear();
							Value.m_Stages.f_Clear();
						}

						if (Notification.m_UpdateID)
							Value.m_bDeferred = true;

						Value.m_LastNotification = Notification;

						auto &OutStage = Value.m_Stages[Notification.m_Stage];
						OutStage.m_Time = Notification.m_UpdateTime;

						WriteCursor.f_Upsert(Key, Value);

						StateValuePromise.f_SetResult(Value);

						auto pAppManager = mp_This.mp_AppManagers.f_FindEqual(_AppManagerID);

						if (pAppManager)
						{
							pAppManager->m_Data.m_LastSeenUpdateNotificationSequence = Notification.m_UniqueSequence;
							CAppManagerKey AppManagerKey{.m_HostID = _AppManagerID};
							WriteCursor.f_Upsert(AppManagerKey, pAppManager->m_Data);
						}

						co_return fg_Move(WriteTransaction);
					}
				)
				.f_Wrap()
			;

			if (!WriteResult)
			{
				DMibLogWithCategory(CloudManager, Critical, "Error saving app manager update state to database: {}", WriteResult.f_GetExceptionStr());
				co_return WriteResult.f_GetException();
			}
		}

		auto UpdateState = co_await fg_Move(StateValueFuture);

		if (!_Notification.m_UpdateID)
		{
			co_await fg_CallSafe(*this, &CUpdateNotifications::fp_ReportApplicationUpdateToSlack, _AppManagerID, UpdateState).f_Wrap()
				> fg_LogError("CloudManager", "Error sending immediate sensor notification")
			;
		}
		else
		{
			auto Mapping = mp_DeferredUpdates.m_AppManagers[_AppManagerID].m_Updates(_Notification.m_UpdateID);
			auto &Update = *Mapping;
			if (Mapping.f_WasCreated())
				mp_DeferredUpdates.m_OrderedUpdates.f_Insert(Update);

			Update.m_AppManagerID = _AppManagerID;
			Update.m_LastUpdateState = fg_Move(UpdateState);
			fp_ScheduleDeferredNotifications();
		}

		co_return {};
	}

	TCFuture<TCMap<CStr, CStr>> CUpdateNotifications::fp_ReportApplicationUpdateToSlack
		(
			CStr const &_AppManagerID
			, CApplicationUpdateStateValue const &_UpdateState
		)
	{
		auto pAppManager = mp_This.mp_AppManagers.f_FindEqual(_AppManagerID);

		if (!pAppManager)
			co_return {};

		CAppManagerInterface::CUpdateNotification const &Notification = _UpdateState.m_LastNotification;

		CSlackActor::CMessage Message;

		auto &SlackAttachment = Message.m_Attachments.f_Insert();

		fp64 StageDuration = fp64::fs_Inf();

		bool bFinished = false;

		CNotifications::EType SlackFlags = CNotifications::EType_None;

		if (Notification.m_Stage == CAppManagerInterface::EUpdateStage_Finished)
		{
			SlackAttachment.m_Title = "Update successful";
			SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Good;
			bFinished = true;
		}
		else if (Notification.m_Stage == CAppManagerInterface::EUpdateStage_Failed)
		{
			SlackAttachment.m_Title = "Update failed";
			SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Danger;
			SlackFlags |= CNotifications::EType_Alert;
			bFinished = true;
		}
		else
			SlackAttachment.m_Title = "Updating ({})"_f << CAppManagerInterface::fs_UpdateStageToStr(Notification.m_Stage);

		if (Notification.m_Message)
			fg_AddStrSep(SlackAttachment.m_Text, "```{}```"_f << CSlackActor::CMessage::fs_EscapeString(Notification.m_Message), "\n\n");

		CStr TimingString;

		fp64 PreviousTime = 0.0;
		CAppManagerInterface::EUpdateStage PreviousStage = CAppManagerInterface::EUpdateStage_None;
		for (auto &StageState : _UpdateState.m_Stages)
		{
			auto &Stage = _UpdateState.m_Stages.fs_GetKey(StageState);

			fp64 PreviousStageTime = (StageState.m_Time - PreviousTime);
			if
				(
					(PreviousStage != CAppManagerInterface::EUpdateStage_None)
					&& ((PreviousStageTime > mp_UpdateStageNotificationThreshold) || (!bFinished && Stage == Notification.m_Stage))
				)
			{
				TimingString += "{cc,sj30,a-}{}\n"_f << CAppManagerInterface::fs_UpdateStageToStr(PreviousStage) << fg_SecondsDurationToHumanReadable(PreviousStageTime);
			}
			PreviousTime = StageState.m_Time;
			PreviousStage = Stage;
		}

		if (!bFinished)
			TimingString += "{cc,sj30,a-}...\n"_f << CAppManagerInterface::fs_UpdateStageToStr(Notification.m_Stage);

		if (TimingString)
			fg_AddStrSep(SlackAttachment.m_Text, "```\n{}```"_f << CSlackActor::CMessage::fs_EscapeString(TimingString), "\n\n");

		auto &AppManagerInfo = pAppManager->m_Data.m_Info;

		SlackAttachment.m_bFieldsMarkdown = true;
		SlackAttachment.m_Fields.f_Insert
			(
				{
					{
						"Environment"
						, AppManagerInfo.m_Environment
						, true
					}
					,
					{
						"Application"
						, "`{}`"_f << CSlackActor::CMessage::fs_EscapeString(Notification.m_Application)
						, true
					}
					,
					{
						"Server"
						, "{}\n{}"_f
						<< CSlackActor::CMessage::fs_EscapeString(AppManagerInfo.m_HostName)
						<< CSlackActor::CMessage::fs_EscapeString(AppManagerInfo.m_ProgramDirectory)
						, true
					}
					,
					{
						"Version"
						, "`{}/{}.{}.{}`\n{}"_f
						<< CSlackActor::CMessage::fs_EscapeString(Notification.m_VersionID.m_VersionID.m_Branch)
						<< Notification.m_VersionID.m_VersionID.m_Major
						<< Notification.m_VersionID.m_VersionID.m_Minor
						<< Notification.m_VersionID.m_VersionID.m_Revision
						<< CSlackActor::CMessage::fs_EscapeString(Notification.m_VersionID.m_Platform)
						, true
					}
					,
					{
						"Total Duration"
						, fg_SecondsDurationToHumanReadable(Notification.m_UpdateTime)
						, true
					}
					,
					{
						"Version Time"
						, "{tc5}"_f
						<< Notification.m_VersionTime
						, true
					}
				}
			)
		;

		if (Notification.m_StartUpdateTime.f_IsValid())
			SlackAttachment.m_FooterTimestamp = Notification.m_StartUpdateTime + CTimeSpanConvert::fs_CreateSpanFromSeconds(Notification.m_UpdateTime);

		auto SlackTimestamps = co_await mp_This.mp_Notifications.f_PostSlackMessage(CNotifications::EType_Update | SlackFlags, Message, _UpdateState.m_SlackTimestamps);

		if (bFinished && Notification.m_UpdateTime > mp_UpdateLongRunningThreshold)
		{
			CSlackActor::CMessage LongRunningMessage;
			LongRunningMessage.m_Text = "Long running update of `{}` on `{}` finished"_f
				<< CSlackActor::CMessage::fs_EscapeString(Notification.m_Application)
				<< CSlackActor::CMessage::fs_EscapeString(AppManagerInfo.m_HostName)
			;
			LongRunningMessage.m_bReplyBroadcast = true;

			auto &LongRunningSlackAttachment = LongRunningMessage.m_Attachments.f_Insert();
			LongRunningSlackAttachment.m_Title = SlackAttachment.m_Title;
			LongRunningSlackAttachment.m_Color = SlackAttachment.m_Color;

			co_await mp_This.mp_Notifications.f_PostSlackMessage(CNotifications::EType_Update, LongRunningMessage, SlackTimestamps);
		}

		{
			auto WriteResult = co_await mp_This.mp_DatabaseActor
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [this, Notification, _AppManagerID, SlackTimestamps = fg_Move(SlackTimestamps)]
					(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) mutable -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						co_await ECoroutineFlag_CaptureMalterlibExceptions;

						auto WriteTransaction = fg_Move(_Transaction);
						if (_bCompacting)
							WriteTransaction = co_await mp_This.self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

						auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();

						CApplicationUpdateStateKey Key;
						Key.m_AppManagerHostID = _AppManagerID;
						Key.m_Application = Notification.m_Application;

						CApplicationUpdateStateValue Value;
						if (WriteCursor.f_FindEqual(Key))
							Value = WriteCursor.f_Value<CApplicationUpdateStateValue>();

						if (Value.m_LastUpdateID == Notification.m_UpdateID)
						{
							if (Value.m_LastNotification.f_IsDone())
								WriteCursor.f_Delete();
							else
							{
								Value.m_SlackTimestamps = fg_Move(SlackTimestamps);
								Value.m_bDeferred = false;
								WriteCursor.f_Upsert(Key, Value);
							}
						}

						co_return fg_Move(WriteTransaction);
					}
				)
				.f_Wrap()
			;

			if (!WriteResult)
			{
				DMibLogWithCategory(CloudManager, Critical, "Error saving app manager update state (Slack timestamp) to database: {}", WriteResult.f_GetExceptionStr());
				co_return WriteResult.f_GetException();
			}
		}

		co_return fg_Move(SlackTimestamps);
	}
}
