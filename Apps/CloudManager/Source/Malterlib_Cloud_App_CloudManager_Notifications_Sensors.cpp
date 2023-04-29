// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	constexpr double gc_MinProblemUpdateTime = 5.0;
	constexpr double gc_ProblemTemporaryTime = 10.0;

	static_assert(gc_MinProblemUpdateTime <= gc_ProblemTemporaryTime);

	CSensorNotifications::CSensorNotifications(CCloudManagerServer &_This)
		: mp_This(_This)
	{
	}

	TCFuture<void> CSensorNotifications::f_Init()
	{
		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		mp_SensorAlertThreshold = fg_Const(mp_This.mp_AppState.m_ConfigDatabase.m_Data).f_GetMemberValue("SensorAlertThreshold", mp_SensorAlertThreshold).f_Float();
		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Error reading notification state from database");

			auto ReadTransaction = co_await mp_This.mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

			for (auto iState = ReadTransaction.m_Transaction.f_ReadCursor(NStr::CStr(), CSensorNotificationStateKey::mc_Prefix); iState; ++iState)
			{
				auto Key = iState.f_Key<CSensorNotificationStateKey>();
				auto Value = iState.f_Value<CSensorNotificationStateValue>();

				auto &SensorStatus = mp_SensorStatuses[Key.f_SensorInfoKey()];
				SensorStatus.m_State = fg_Move(Value);

				if (SensorStatus.m_State.m_bInProblemState)
				{
					SensorStatus.m_ProblemClock.f_Start();
					SensorStatus.m_ProblemClock.f_AddOffset(-SensorStatus.m_State.m_TimeInProblemState); // Could be off by the time the CloudManager was down
				}
			}
		}

		mp_SensorSubscription = co_await mp_This.mp_AppSensorStore
			(
				&CDistributedAppSensorStoreLocal::f_SubscribeSensors
				, TCVector<CDistributedAppSensorReader_SensorFilter>()
				, g_ActorFunctor / [this](CDistributedAppSensorReader::CSensorChange &&_Change) -> TCFuture<void>
				{
					switch (_Change.f_GetTypeID())
					{
					case CDistributedAppSensorReader::ESensorChange_AddedOrUpdated:
						{
							auto &Sensor = _Change.f_Get<CDistributedAppSensorReader::ESensorChange_AddedOrUpdated>();
							auto SensorKey = Sensor.f_Key();
							auto &SensorStatus = mp_SensorStatuses[SensorKey];
							SensorStatus.m_Info = fg_Move(Sensor);
							SensorStatus.m_bInfoValid = true;
							mp_RemovedSensors.f_Remove(SensorKey);
						}
						break;
					case CDistributedAppSensorReader::ESensorChange_Removed:
						{
							auto &SensorKey = _Change.f_Get<CDistributedAppSensorReader::ESensorChange_Removed>();
							mp_SensorStatuses.f_Remove(SensorKey);
							mp_RemovedSensors[SensorKey];
						}
						break;
					}

					co_return {};
				}
			)
		;

		mp_SensorStatusSubscription = co_await mp_This.mp_AppSensorStore
			(
				&CDistributedAppSensorStoreLocal::f_SubscribeSensorStatus
				, TCVector<CDistributedAppSensorReader_SensorStatusFilter>()
				, g_ActorFunctor / [this](CDistributedAppSensorReader_SensorKeyAndReading &&_Reading) -> TCFuture<void>
				{
					auto &SensorStatus = mp_SensorStatuses[_Reading.m_SensorInfoKey];

					SensorStatus.m_LastReading = fg_Move(_Reading.m_Reading);
					if (mp_bSubscribedToSensors)
						co_await fp_UpdateSensorFromReading(_Reading.m_SensorInfoKey, CTime::fs_NowUTC());

					co_return {};
				}
			)
		;

		fg_Timeout(2.0) > [this]
			{
				// Allow time for sersors to recorrect after startup before considering their status
				mp_bSubscribedToSensors = true;
				fp_SchedulePeriodicSensorNotificationsOutOfBand();
			}
		;

		co_return {};
	}

	TCFuture<void> CSensorNotifications::f_Destroy()
	{
		co_await fg_Move(mp_UpdateSequencer).f_Destroy();

		if (mp_SensorSubscription)
			co_await fg_Exchange(mp_SensorSubscription, nullptr)->f_Destroy();
		if (mp_SensorStatusSubscription)
			co_await fg_Exchange(mp_SensorStatusSubscription, nullptr)->f_Destroy();

		co_return {};
	}

	CDistributedAppSensorReporter::CSensorInfo const *CSensorNotifications::CSensorStatus::f_GetInfo()
	{
		if (m_bInfoValid)
			return &m_Info;
		return nullptr;
	}

	void CSensorNotifications::fp_SchedulePeriodicSensorNotificationsOutOfBand()
	{
		if (mp_bSensorsUpdating)
		{
			mp_bSensorsReschedule = true;
			return;
		}

		if (mp_bOutOfBandSensorsUpdateScheduled)
			return;

		mp_bOutOfBandSensorsUpdateScheduled = true;
		fg_Timeout(gc_MinProblemUpdateTime) > [this]()
			{
				mp_bOutOfBandSensorsUpdateScheduled = false;
				f_UpdatePeriodicSensorNotifications() > fg_LogError("CloudManager", "Error updating sensor notifications");
			}
		;
	}

	TCFuture<void> CSensorNotifications::fp_UpdateSensorFromReading(CDistributedAppSensorReporter::CSensorInfoKey _SensorKey, CTime _Now)
	{
		CSensorStatus *pSensorStatus = nullptr;

		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (mp_This.f_IsDestroyed())
						return DMibErrorInstance("Shutting down");

					pSensorStatus = mp_SensorStatuses.f_FindEqual(_SensorKey);
					if (!pSensorStatus)
						return DMibErrorInstance("Status removed");

					return {};
				}
			)
		;

		auto pSensorInfo = pSensorStatus->f_GetInfo();

		if (!pSensorInfo)
			co_return {};

		auto CombinedStatus = pSensorStatus->m_LastReading.f_GetCombinedStatus(pSensorInfo, _Now);
		auto LastCombinedStatus = pSensorStatus->m_LastCombinedStatus;
		auto bLastCombinedStatusValid = pSensorStatus->m_bLastCombinedStatusValid;
		pSensorStatus->m_LastCombinedStatus = CombinedStatus;
		pSensorStatus->m_bLastCombinedStatusValid = true;

		bool bForceUpdate = false;

		if (CombinedStatus > CDistributedAppSensorReporter::EStatusSeverity_Warning && !pSensorStatus->m_State.m_bInProblemState)
		{
			bForceUpdate = true;
			// Signal problem state as fast as possible
			pSensorStatus->m_State.m_bInProblemState = true;
			pSensorStatus->m_State.m_TimeInProblemState = 0.0;
			pSensorStatus->m_ProblemClock.f_Start();
		}

		if (!bLastCombinedStatusValid || CombinedStatus != LastCombinedStatus || bForceUpdate)
			fp_SchedulePeriodicSensorNotificationsOutOfBand();

		co_return {};
	}

	TCFuture<void> CSensorNotifications::f_UpdatePeriodicSensorNotifications()
	{
		if (!mp_bSubscribedToSensors)
			co_return {};

		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		if (mp_bSensorsUpdating)
		{
			mp_bSensorsReschedule = true;
			co_return {};
		}

		auto SequenceSubscription = co_await mp_UpdateSequencer.f_Sequence();

		mp_bSensorsUpdating = true;
		auto Cleanup = g_OnScopeExit / [&]
			{
				mp_bSensorsUpdating = false;
				if (fg_Exchange(mp_bSensorsReschedule, false))
					fp_SchedulePeriodicSensorNotificationsOutOfBand();
			}
		;

		auto &GlobalState = mp_This.mp_GlobalState;

 		NTime::CTime Now = NTime::CTime::fs_NowUTC();

		TCVector<CSlackActor::CAttachment> ProblemAttachments;
		TCVector<CSlackActor::CAttachment> FixedAttachments;
		TCMap<CDistributedAppSensorReporter::CSensorInfoKey, TCSet<CStr>> TemporaryProblems;

		CNotifications::EType NotificationFlags = CNotifications::EType_None;

		bool bReSchedule = false;

		auto fEscape = [](CStr const &_String)
			{
				return CSlackActor::CMessage::fs_EscapeString(_String);
			}
		;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Error reading known hosts from database");

			TCOptional<CDatabaseActor::CTransactionRead> ReadTransaction;

			for (auto &Sensor : mp_SensorStatuses)
			{
				if (!Sensor.m_bInfoValid)
					continue;

				auto &SensorInfoKey = mp_SensorStatuses.fs_GetKey(Sensor);
				auto &SensorInfo = Sensor.m_Info;
				bool bRemoved = SensorInfo.m_bRemoved;

				if (!bRemoved && !SensorInfoKey.m_HostID.f_IsEmpty())
				{
					if (!ReadTransaction)
						ReadTransaction = co_await mp_This.mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

					NSensorStoreLocalDatabase::CKnownHostKey Key{.m_DbPrefix = CCloudManagerServer::mc_DatabasePrefixSensor, .m_HostID = SensorInfoKey.m_HostID};
					NSensorStoreLocalDatabase::CKnownHostValue Value;
					if (ReadTransaction->m_Transaction.f_Get(Key, Value))
					{
						if (Value.m_bRemoved)
							bRemoved = true;
					}
				}

				bool bIsTemporary = false;
				bool bIsFixed = false;

				auto &Reading = Sensor.m_LastReading;
				auto CombinedStatus = Reading.f_GetCombinedStatus(&SensorInfo, Now);

				if (CombinedStatus < CDistributedAppSensorReporter::EStatusSeverity_Warning || bRemoved)
				{
					if (Sensor.m_State.m_bInProblemState)
					{
						Sensor.m_State.m_bInProblemState = false;
						if (Sensor.m_State.m_TimeInProblemState >= gc_ProblemTemporaryTime)
							bIsFixed = true;
						else
							bIsTemporary = true;
						Sensor.m_State.m_TimeInProblemState = Sensor.m_ProblemClock.f_GetTime();
					}
					else
						continue;
				}
				else
				{
					if (!Sensor.m_State.m_bInProblemState)
					{
						Sensor.m_State.m_bInProblemState = true;
						Sensor.m_State.m_TimeInProblemState = 0.0;
						Sensor.m_ProblemClock.f_Start();
						bReSchedule = true;
						continue;
					}
					else
					{
						Sensor.m_State.m_TimeInProblemState = Sensor.m_ProblemClock.f_GetTime();
						if (Sensor.m_State.m_TimeInProblemState < gc_ProblemTemporaryTime)
						{
							bReSchedule = true;
							continue;
						}

						if
							(
								CombinedStatus >= CDistributedAppSensorReporter::EStatusSeverity_Error
								&& Sensor.m_State.m_TimeInProblemState > mp_SensorAlertThreshold
								&& !Sensor.m_State.m_bSentAlert
							)
						{
							Sensor.m_State.m_bSentAlert = true;
							NotificationFlags |= CNotifications::EType_Alert;
						}
					}
				}

				if (bIsTemporary)
				{
					TemporaryProblems[SensorInfoKey][SensorInfo.m_Name];
					continue;
				}

				auto &Attachments = [&]() -> TCVector<CSlackActor::CAttachment> &
					{
						if (bIsFixed)
							return FixedAttachments;
						else
							return ProblemAttachments;
					}
					()
				;

				auto &SlackAttachment = Attachments.f_Insert();

				SlackAttachment.m_bFieldsMarkdown = true;
				SlackAttachment.m_bTextMarkdown = true;

				SlackAttachment.m_Text = "`{}`"_f << fEscape(SensorInfo.m_Name);
				SlackAttachment.m_Footer = SensorInfo.m_HostName;

				if (bRemoved)
					;
				else if (CombinedStatus >= CDistributedAppSensorReporter::EStatusSeverity_Error)
					SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Danger;
				else if (CombinedStatus >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
					SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Warning;
				else
					SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Good;

				CSensorNotificationStateNotification Notification;
				Notification.m_Status.m_Severity = Reading.f_Status();

				fp64 OutdatedSeconds;
				Notification.m_OutdatedStatus.m_Severity = Reading.f_OutdatedStatus(SensorInfo, Now, OutdatedSeconds);
				Notification.m_CriticalityStatus.m_Severity = Reading.f_CriticalityStatus(SensorInfo);

				if (Notification.m_Status.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning && Reading.m_Data.f_IsOfType<CDistributedAppSensorReporter::CStatus>())
					Notification.m_Status.m_Message = fEscape(Reading.m_Data.f_GetAsType<CDistributedAppSensorReporter::CStatus>().m_Description);

				if (Notification.m_CriticalityStatus.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
					Notification.m_CriticalityStatus.m_Message = Reading.f_GetFormattedData(EAnsiEncodingFlag_None, &SensorInfo);

				if (Notification.m_OutdatedStatus.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
				{
					Notification.m_OutdatedStatus.m_Message = Reading.f_GetFormattedOutdatedStatus
						(
							EAnsiEncodingFlag_None
							, &SensorInfo
							, Now
							, CDistributedAppSensorReporter::EOutdatedStatusOutputFlag_None
						)
					;
				}

				auto &LastNotification = Sensor.m_State.m_LastNotification;

				auto fAddField = [&](CSensorNotificationStateNotificationStatus &_Status, bool _bShort)
					{
						auto &Field = SlackAttachment.m_Fields.f_Insert();
						Field.m_Value = "*{}*\n```{}```"_f
							<< CDistributedAppSensorReporter::fs_StatusSeverityToString(_Status.m_Severity)
							<< fEscape(_Status.m_Message)
						;
						Field.m_bShort = _bShort;
					}
				;

				auto fAddFields = [&](CSensorNotificationStateNotificationStatus &_Status, CSensorNotificationStateNotificationStatus &_LastStatus)
					{
						if (_Status.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning && !bRemoved)
							fAddField(_Status, false);
						else if (_LastStatus.m_Severity >= CDistributedAppSensorReporter::EStatusSeverity_Warning)
						{
							fAddField(_LastStatus, true);
							auto &Field = SlackAttachment.m_Fields.f_Insert();

							CStr FixType = "Resolved";
							if (bRemoved)
								FixType = "Sensor removed";

							Field.m_Value = "*{0}*\n```{0} after {1}```"_f << FixType << fg_SecondsDurationToHumanReadable(Sensor.m_State.m_TimeInProblemState);
							Field.m_bShort = true;
						}
					}
				;

				fAddFields(Notification.m_Status, LastNotification.m_Status);
				fAddFields(Notification.m_CriticalityStatus, LastNotification.m_CriticalityStatus);
				fAddFields(Notification.m_OutdatedStatus, LastNotification.m_OutdatedStatus);

				SlackAttachment.m_FooterTimestamp = Reading.m_Timestamp;

				Sensor.m_State.m_LastNotification = fg_Move(Notification);
			}
		}

		bool bProblemsFixed = false;

		if (!FixedAttachments.f_IsEmpty())
		{
			CSlackActor::CMessage Message;
			Message.m_Text = "*Resolved sensor problems*";
			Message.m_bMarkdown = true;
			Message.m_Attachments = fg_Move(FixedAttachments);

			CNotifications::fs_LimitMessage(Message, "sensor problems were resolved");

			GlobalState.m_SensorProblemsSlackThread = co_await mp_This.mp_Notifications.f_PostSlackMessage
				(
					CNotifications::EType_Sensor
					, Message
					, fg_Move(GlobalState.m_SensorProblemsSlackThread)
				)
			;
			bProblemsFixed = true;
		}

		if (!TemporaryProblems.f_IsEmpty())
		{
			CSlackActor::CMessage Message;
			Message.m_Text = "*Temporary sensor problems*\nThe following sensors were temporarily in an error state:";
			Message.m_bMarkdown = true;

			for (auto &Names : TemporaryProblems)
			{
				auto &SensorInfoKey = TemporaryProblems.fs_GetKey(Names);
				auto &Attachment = Message.m_Attachments.f_Insert();
				auto *pSensor = mp_SensorStatuses.f_FindEqual(TemporaryProblems.fs_GetKey(Names));
				DMibFastCheck(pSensor);
				if (!pSensor)
					continue;
				auto pSensorInfo = pSensor->f_GetInfo();
				CStr Hostname = pSensorInfo ? pSensorInfo->m_HostName : CStr("Unknown host ({})"_f << SensorInfoKey.m_HostID);

				Attachment.m_Text = "{}: `{}`"_f << fEscape(Hostname) << fEscape(CStr::fs_Join(TCVector<CStr>::fs_FromContainer(Names), ", "));
			}

			CNotifications::fs_LimitMessage(Message, "hosts temporarily had a problem");

			co_await mp_This.mp_Notifications.f_PostSlackMessage(CNotifications::EType_Sensor, Message, fg_Default());
		}

		if (!ProblemAttachments.f_IsEmpty())
		{
			CSlackActor::CMessage Message;
			Message.m_Text = "*Sensor problems*";
			Message.m_bMarkdown = true;
			Message.m_Attachments = fg_Move(ProblemAttachments);
			if (bProblemsFixed)
				Message.m_bReplyBroadcast = true;

			CNotifications::fs_LimitMessage(Message, "sensors are in a problem state");

			if (Message != mp_LastSentProblemMessage || NotificationFlags != CNotifications::EType_None)
			{
				mp_LastSentProblemMessage = Message;
				GlobalState.m_SensorProblemsSlackThread = co_await mp_This.mp_Notifications.f_PostSlackMessage
					(
						CNotifications::EType_Sensor | NotificationFlags
						, Message
						, fg_Move(GlobalState.m_SensorProblemsSlackThread)
					)
				;
			}
		}
		else if (bProblemsFixed)
		{
			CSlackActor::CMessage AllResolvedMessage;
			AllResolvedMessage.m_Text = "*All sensor problems have now been resolved*";
			AllResolvedMessage.m_bReplyBroadcast = true;

			co_await mp_This.mp_Notifications.f_PostSlackMessage
				(
					CNotifications::EType_Sensor
					, AllResolvedMessage
					, fg_Move(GlobalState.m_SensorProblemsSlackThread)
				)
			;
		}
		else
			GlobalState.m_SensorProblemsSlackThread.f_Clear();

		auto Result = co_await mp_This.mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await mp_This.self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					WriteTransaction = co_await mp_This.fp_SaveGlobalState(fg_Move(WriteTransaction));

					for (auto &RemovedSensorKey : mp_RemovedSensors)
					{
						auto DatabasKey = fg_GetSensorDatabaseKey<CSensorNotificationStateKey>(RemovedSensorKey);

						if (WriteTransaction.m_Transaction.f_Exists(DatabasKey))
							WriteTransaction.m_Transaction.f_Delete(DatabasKey);
					}

					for (auto &SensorStatus : mp_SensorStatuses)
					{
						auto DatabasKey = fg_GetSensorDatabaseKey<CSensorNotificationStateKey>(mp_SensorStatuses.fs_GetKey(SensorStatus));
						WriteTransaction.m_Transaction.f_Upsert(DatabasKey, SensorStatus.m_State);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving sensor notification state to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		if (bReSchedule)
			fp_SchedulePeriodicSensorNotificationsOutOfBand();

		co_return {};
	}
}
