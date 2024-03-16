// Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/DistributedAppLogFilter>

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	constexpr double gc_LogAggregationTime = 10.0;

	CLogNotifications::CLogNotifications(CCloudManagerServer &_This)
		: mp_This(_This)
	{
	}

	TCFuture<void> CLogNotifications::f_Init()
	{
		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		mp_LastSeenTimestamp = mp_This.mp_GlobalState.m_LastSeenLogTimestamp;

		auto fAddConfig = [&]() -> CAlertConfig &
			{
				auto &Config = mp_AlertConfigs.f_Insert();

				if (mp_LastSeenTimestamp.f_IsValid())
					Config.m_Filter.m_MinTimestamp = mp_LastSeenTimestamp;

				Config.m_Filter.m_LogFilter.m_Flags |= CDistributedAppLogReader_LogFilter::ELogFlag_IgnoreRemoved;

				return Config;
			}
		;

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing log notification config");

			auto &Config = fg_Const(mp_This.mp_AppState.m_ConfigDatabase.m_Data);

			auto *pLogAlerts = Config.f_GetMember("LogAlerts");
			if (pLogAlerts)
			{
				for (auto &Alert : pLogAlerts->f_Array())
				{
					auto &Config = fAddConfig();
					auto &Filter = Config.m_Filter;

					for (auto &ConfigValue : Alert.f_Object())
					{
						if (ConfigValue.f_Name() == "AlertMessage")
							Config.m_Message = ConfigValue.f_Value().f_String();
						else if (ConfigValue.f_Name() == "AlertSeverity")
						{
							auto &SeverityName = ConfigValue.f_Value().f_String();
							if (SeverityName == "Critical")
							{
								Config.m_SlackColor = CSlackActor::EPredefinedColor_Danger;
								Config.m_TypeFlags = CNotifications::EType_Alert;
							}
							else if (SeverityName == "Error")
								Config.m_SlackColor = CSlackActor::EPredefinedColor_Danger;
							else if (SeverityName == "Warning")
								Config.m_SlackColor = CSlackActor::EPredefinedColor_Warning;
							else if (SeverityName == "Ok")
								Config.m_SlackColor = CSlackActor::EPredefinedColor_Good;
							else if (SeverityName == "Info")
								;
							else
								DMibError("Unknown alert severity: {}"_f << SeverityName);
						}
						else if (ConfigValue.f_Name() == "HostID")
							Filter.m_LogFilter.m_HostID = ConfigValue.f_Value().f_String();
						else if (ConfigValue.f_Name() == "Application")
							Filter.m_LogFilter.m_Scope = CDistributedAppLogReporter::CLogScope_Application{.m_ApplicationName = ConfigValue.f_Value().f_String()};
						else if (ConfigValue.f_Name() == "Identifier")
							Filter.m_LogFilter.m_Identifier = ConfigValue.f_Value().f_String();
						else if (ConfigValue.f_Name() == "IdentifierScope")
							Filter.m_LogFilter.m_IdentifierScope = ConfigValue.f_Value().f_String();
						else if (ConfigValue.f_Name() == "CategoriesWildcards")
							Filter.m_LogDataFilter.m_CategoriesWildcards = TCSet<CStr>::fs_FromContainer(ConfigValue.f_Value().f_StringArray());
						else if (ConfigValue.f_Name() == "OperationsWildcards")
							Filter.m_LogDataFilter.m_OperationsWildcards = TCSet<CStr>::fs_FromContainer(ConfigValue.f_Value().f_StringArray());
						else if (ConfigValue.f_Name() == "MessageWildcards")
							Filter.m_LogDataFilter.m_MessageWildcards = TCSet<CStr>::fs_FromContainer(ConfigValue.f_Value().f_StringArray());
						else if (ConfigValue.f_Name() == "SourceLocationFileWildcards")
							Filter.m_LogDataFilter.m_SourceLocationFileWildcards = TCSet<CStr>::fs_FromContainer(ConfigValue.f_Value().f_StringArray());
						else if (ConfigValue.f_Name() == "Severities")
						{
							NContainer::TCSet<CDistributedAppLogReporter::ELogSeverity> Severities;
							for (auto &SeverityJSON : ConfigValue.f_Value().f_Array())
							{
								auto SeverityString = SeverityJSON.f_String();

								CDistributedAppLogReporter::ELogSeverity Severity = CDistributedAppLogReporter::fs_LogSeverityFromStr(SeverityString);
								if (Severity == CDistributedAppLogReporter::ELogSeverity_Unsupported)
									DMibError("Unknown severity: {}"_f << SeverityString);

								Severities[Severity];
							}

							Filter.m_LogDataFilter.m_Severities = fg_Move(Severities);
						}
						else if (ConfigValue.f_Name() == "Flags")
						{
							CDistributedAppLogReporter::ELogEntryFlag Flags = CDistributedAppLogReporter::ELogEntryFlag_None;
							for (auto &Flag : ConfigValue.f_Value().f_Array())
							{
								auto &FlagString = Flag.f_String();

								if (FlagString == "Audit")
									Flags |= CDistributedAppLogReporter::ELogEntryFlag_Audit;
								else if (FlagString == "Performance")
									Flags |= CDistributedAppLogReporter::ELogEntryFlag_Performance;
								else
									DMibError("Unknown flag: {}"_f << FlagString);
							}

							Filter.m_LogDataFilter.m_Flags = Flags;
 						}
					}
				}
			}
		}

		if (mp_AlertConfigs.f_IsEmpty())
			co_return {};

		NContainer::TCVector<CDistributedAppLogReader_LogEntrySubscriptionFilter> LogEntryFilters;

		for (auto &Config : mp_AlertConfigs)
			LogEntryFilters.f_Insert(Config.m_Filter);

		mp_LogSubscription = co_await mp_This.mp_AppLogStore
			(
				&CDistributedAppLogStoreLocal::f_SubscribeLogs
				, TCVector<CDistributedAppLogReader_LogFilter>()
				, g_ActorFunctor / [this](CDistributedAppLogReader::CLogChange &&_Change) -> TCFuture<void>
				{
					switch (_Change.f_GetTypeID())
					{
					case CDistributedAppLogReader::ELogChange_AddedOrUpdated:
						{
							auto &Log = _Change.f_Get<CDistributedAppLogReader::ELogChange_AddedOrUpdated>();
							auto LogKey = Log.f_Key();
							auto &LogStatus = mp_LogStatuses[LogKey];
							LogStatus.m_Info = fg_Move(Log);
							LogStatus.m_bInfoValid = true;
						}
						break;
					case CDistributedAppLogReader::ELogChange_Removed:
						{
							auto &LogKey = _Change.f_Get<CDistributedAppLogReader::ELogChange_Removed>();
							mp_LogStatuses.f_Remove(LogKey);
						}
						break;
					}

					co_return {};
				}
			)
		;

		CDistributedAppLogReader::CSubscribeLogEntries SubscribeLogEntriesParams;
		SubscribeLogEntriesParams.m_Flags = CDistributedAppLogReader::ELogEntriesFlag_IncludeLastSeenSentinel;
		SubscribeLogEntriesParams.m_Filters = fg_Move(LogEntryFilters);
		SubscribeLogEntriesParams.m_fOnEntry = g_ActorFunctor / [this](CDistributedAppLogReader_LogKeyAndEntry &&_Entry) -> TCFuture<void>
			{
				CStr ThisHostID;
				NLogStore::CFilterLogKeyContext FilterContext{.m_ThisHostID = ThisHostID, .m_Prefix = mp_This.mc_DatabasePrefixLog};

				mp_LastSeenTimestamp = fg_Max(mp_LastSeenTimestamp, _Entry.m_Entry.m_Timestamp);

				if (_Entry.f_IsLastSeenSentinel())
				{
					fp_ScheduleSlackNotification();
					co_return {};
				}

				auto DatabaseLogKey = fg_GetLogDatabaseKey<NLogStoreLocalDatabase::CLogKey>(_Entry.m_LogInfoKey);

				for (auto &AlertConfig : mp_AlertConfigs)
				{
					auto *pLog = mp_LogStatuses.f_FindEqual(_Entry.m_LogInfoKey);

					// We don't need to send in known hosts here, because the log filters already have ELogFlag_IgnoreRemoved set.
					if (!NLogStore::fg_FilterLogKey(DatabaseLogKey, AlertConfig.m_Filter.m_LogFilter, FilterContext, pLog ? pLog->f_GetInfo() : nullptr, nullptr))
						continue;

					if (!NLogStore::fg_FilterLogValue(_Entry.m_Entry.m_Data, AlertConfig.m_Filter.m_LogDataFilter))
						continue;

					AlertConfig.m_QueuedEntries.f_Insert(fg_Move(_Entry));

					fp_ScheduleSlackNotification();

					co_return {};
				}

				co_return {};
			}
		;

		mp_LogEntriesSubscription = co_await mp_This.mp_AppLogStore(&CDistributedAppLogStoreLocal::f_SubscribeLogEntries, fg_Move(SubscribeLogEntriesParams));

		mp_bSubscribedToLogs = true;

		co_return {};
	}

	TCFuture<void> CLogNotifications::f_Destroy()
	{
		co_await fg_Move(mp_UpdateSequencer).f_Destroy();

		if (mp_LogSubscription)
			co_await fg_Exchange(mp_LogSubscription, nullptr)->f_Destroy();
		if (mp_LogEntriesSubscription)
			co_await fg_Exchange(mp_LogEntriesSubscription, nullptr)->f_Destroy();

		co_return {};
	}

	CDistributedAppLogReporter::CLogInfo const *CLogNotifications::CLogStatus::f_GetInfo()
	{
		if (m_bInfoValid)
			return &m_Info;
		return nullptr;
	}

	void CLogNotifications::fp_ScheduleSlackNotification()
	{
		if (mp_bSendingSlackNotifications)
		{
			mp_bRescheduleSlackNotifications = true;
			return;
		}

		if (mp_bSlackNotificationsScheduled)
			return;

		mp_bSlackNotificationsScheduled = true;
		fg_Timeout(gc_LogAggregationTime) > [this]()
			{
				mp_bSlackNotificationsScheduled = false;
				fp_SendSlackNotifications() > fg_LogError("CloudManager", "Error sending log notifications");
			}
		;
	}

	namespace
	{
		struct CAggregatedLogData
		{
			CDistributedAppLogReader_LogKeyAndEntry *m_pFirstLogEntry = nullptr;
			CDistributedAppLogReader_LogKeyAndEntry *m_pLastLogEntry = nullptr;
			mint m_nMessages = 0;
			DLinkDS_Link(CAggregatedLogData, m_Link);
		};

		struct CAggregatedLog
		{
			CDistributedAppLogReporter::ELogSeverity f_MaxSeverity() const
			{
				CDistributedAppLogReporter::ELogSeverity Severity = CDistributedAppLogReporter::ELogSeverity_DebugVerbose3;

				for (auto &Messages : m_Messages)
				{
					auto &LogData = m_Messages.fs_GetKey(Messages);
					Severity = fg_Min(Severity, LogData.m_Severity);
				}

				return Severity;
			}

			TCMap<CDistributedAppLogReporter::CLogData, CAggregatedLogData> m_Messages;
			DLinkDS_List(CAggregatedLogData, m_Link) m_MessagesOrdered;
		};
	}

	TCFuture<void> CLogNotifications::fp_SendSlackNotifications()
	{
		if (!mp_bSubscribedToLogs)
			co_return {};

		auto OnResume = co_await mp_This.f_CheckDestroyedOnResume();

		if (mp_bSendingSlackNotifications)
		{
			mp_bRescheduleSlackNotifications = true;
			co_return {};
		}

		auto SequenceSubscription = co_await mp_UpdateSequencer.f_Sequence();

		mp_bSendingSlackNotifications = true;
		auto Cleanup = g_OnScopeExit / [&]
			{
				mp_bSendingSlackNotifications = false;
				if (fg_Exchange(mp_bRescheduleSlackNotifications, false))
					fp_ScheduleSlackNotification();
			}
		;

		TCVector<CSlackActor::CAttachment> AlertAttachments;

		auto fEscape = [](CStr const &_String)
			{
				return CSlackActor::CMessage::fs_EscapeString(_String);
			}
		;

		CNotifications::EType NotificationFlags = CNotifications::EType_None;

		for (auto &AlertConfig : mp_AlertConfigs)
		{
			TCMap<CDistributedAppLogReporter::CLogInfoKey, CAggregatedLog> AggregatedLogs;

			for (auto &Entry : AlertConfig.m_QueuedEntries)
			{
				auto &Log = AggregatedLogs[Entry.m_LogInfoKey];
				auto Mapped = Log.m_Messages(Entry.m_Entry.m_Data);
				auto &Message = *Mapped;
				if (Mapped.f_WasCreated())
				{
					Message.m_pFirstLogEntry = &Entry;
					Log.m_MessagesOrdered.f_Insert(Message);
				}
				Message.m_pLastLogEntry = &Entry;

				++Message.m_nMessages;
			}

			for (auto &Log : AggregatedLogs)
			{
				auto &LogInfoKey = AggregatedLogs.fs_GetKey(Log);
				auto *pLogStatus = mp_LogStatuses.f_FindEqual(LogInfoKey);

				if (!pLogStatus)
					continue;

				auto *pLogInfo = pLogStatus->f_GetInfo();

				if (!pLogInfo)
					continue;

				auto MaxSeverity = Log.f_MaxSeverity();

				auto fGetAttachmentForMessage = [&, pAttachment = (CSlackActor::CAttachment *)nullptr]() mutable -> CSlackActor::CAttachment &
					{
						pAttachment = &AlertAttachments.f_Insert();
						auto &Attachment = *pAttachment;

						Attachment.m_bFieldsMarkdown = true;
						Attachment.m_bTextMarkdown = true;

						if (LogInfoKey.m_IdentifierScope)
							Attachment.m_Text = "*{}* ({} {})"_f << fEscape(pLogInfo->m_Name) << LogInfoKey.m_Identifier << LogInfoKey.m_IdentifierScope;
						else
							Attachment.m_Text = "*{}* ({})"_f << fEscape(pLogInfo->m_Name) << LogInfoKey.m_Identifier;

						Attachment.m_Footer = pLogInfo->m_HostName;

						return *pAttachment;
					}
				;

				if (AlertConfig.m_TypeFlags)
					NotificationFlags |= *AlertConfig.m_TypeFlags;
				else if (MaxSeverity == CDistributedAppLogReporter::ELogSeverity_Critical)
					NotificationFlags |= CNotifications::EType_Alert;

				for (auto &Message : Log.m_MessagesOrdered)
				{
					auto &MessageData = Log.m_Messages.fs_GetKey(Message);

					auto &SlackAttachment = fGetAttachmentForMessage();

					SlackAttachment.m_FooterTimestamp = Message.m_pLastLogEntry->m_Entry.m_Timestamp;

					if (AlertConfig.m_SlackColor)
						SlackAttachment.m_Color = *AlertConfig.m_SlackColor;
					else
					{
						switch (MessageData.m_Severity)
						{
						case CDistributedAppLogReporter::ELogSeverity_Critical: SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Danger; break;
						case CDistributedAppLogReporter::ELogSeverity_Error: SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Danger; break;
						case CDistributedAppLogReporter::ELogSeverity_Warning: SlackAttachment.m_Color = CSlackActor::EPredefinedColor_Warning; break;
						default: SlackAttachment.m_Color = CSlackActor::CRgbColor{0,135,215}; break;
						}
					}

					auto &MessageField = SlackAttachment.m_Fields.f_Insert();
					MessageField.m_Title = "`{tc6}`"_f << Message.m_pFirstLogEntry->m_Entry.m_Timestamp;
					if (Message.m_nMessages > 1)
						MessageField.m_Title += " (Repeated {} times)"_f << Message.m_nMessages;
					MessageField.m_Value = CStr::CFormat(AlertConfig.m_Message) << fEscape(MessageData.m_Message);

					CStr SeverityString;
					if (MessageData.m_Flags & CDistributedAppLogReporter::ELogEntryFlag_Audit)
						fg_AddStrSep(SeverityString, "*Audit*", " ");
					if (MessageData.m_Flags & CDistributedAppLogReporter::ELogEntryFlag_Performance)
						fg_AddStrSep(SeverityString, "_Performance_", " ");

					fg_AddStrSep(SeverityString, CDistributedAppLogReporter::fs_LogSeverityToStr(MessageData.m_Severity), " ");

					auto fAddInfoField = [&](CStr const &_Name, auto &&_Value, bool _bShort = true)
						{
							auto &Field = SlackAttachment.m_Fields.f_Insert();
							Field.m_bShort = _bShort;
							Field.m_Title = fEscape(_Name);
							Field.m_Value = _Value;
						}
					;

					fAddInfoField("Severity", SeverityString);
					if (!MessageData.m_Categories.f_IsEmpty())
						fAddInfoField("Categories", fEscape(CStr::fs_Join(MessageData.m_Categories, ", ")));
					if (!MessageData.m_Operations.f_IsEmpty())
						fAddInfoField("Operations", fEscape(CStr::fs_Join(MessageData.m_Operations, ", ")));
					if (MessageData.m_MetaData.f_IsValid())
						fAddInfoField("MetaData", "```{}```"_f << fEscape(CStr::fs_ToStr(MessageData.m_MetaData)), false);
				}
			}

			AlertConfig.m_QueuedEntries.f_Clear();
		}

		if (!AlertAttachments.f_IsEmpty())
		{
			CSlackActor::CMessage Message;
			Message.m_Text = "*Log entries*";
			Message.m_bMarkdown = true;
			Message.m_Attachments = fg_Move(AlertAttachments);

			CNotifications::fs_LimitMessage(Message, "logs from applications exists");

			co_await mp_This.mp_Notifications.f_PostSlackMessage
				(
					CNotifications::EType_Log | NotificationFlags
					, Message
					, fg_Default()
				)
			;
		}

		if (mp_LastSeenTimestamp != mp_This.mp_GlobalState.m_LastSeenLogTimestamp)
		{
			mp_This.mp_GlobalState.m_LastSeenLogTimestamp = mp_LastSeenTimestamp;
			co_await mp_This.fp_SaveGlobalStateWithoutTransaction();
		}

		co_return {};
	}
}
