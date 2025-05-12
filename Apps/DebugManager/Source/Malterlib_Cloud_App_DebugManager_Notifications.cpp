// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	TCFuture<void> CDebugManagerApp::fp_Notify_OpenLogReporter()
	{
		if (mp_NotificationLogReporter)
			co_return {};

		CDistributedAppLogReporter::CLogInfo LogInfo;
		LogInfo.m_Identifier = "org.malterlib.log.crashdumps";
		LogInfo.m_Name = "Malterlib Crash Dumps";
		LogInfo.m_Metadata = mp_LogMetadata;

		mp_NotificationLogReporter = co_await f_OpenLogReporter(fg_Move(LogInfo));

		co_return {};
	}
	
	TCFuture<void> CDebugManagerApp::fp_Notify_ScheduleProcess()
	{
		if (!mp_PendingNotificationsTimeout)
		{
			mp_PendingNotificationsTimeout = co_await fg_RegisterTimer
				(
					1_minutes
					, [this] -> TCFuture<void>
					{
						co_await fp_Notify_Process(false);
						co_return {};
					}
				)
			;
		}

		co_return {};
	}

	TCFuture<void> CDebugManagerApp::fp_Notify_CrashDumpAdded(CDebugDatabase::CCrashDumpAdd _CrashDumpAdd)
	{
		auto &PendingNotification = mp_PendingNotifications[_CrashDumpAdd.m_ID];

		PendingNotification.m_FileNames.f_Insert(_CrashDumpAdd.m_FileName);
		PendingNotification.m_Metadata = _CrashDumpAdd.m_Metadata;

		if (_CrashDumpAdd.m_ExceptionInfo)
			PendingNotification.m_ExceptionInfo = _CrashDumpAdd.m_ExceptionInfo;

		PendingNotification.m_LastAdded.f_Start();

		co_await fp_Notify_ScheduleProcess();

		co_return {};
	}

	TCFuture<void> CDebugManagerApp::fp_Notify_Process(bool _bForce)
	{
		TCSet<CStr> NotificationsToDelete;
		TCVector<CDistributedAppLogReporter::CLogEntry> LogEntries;

		for (auto &PendingNotificationEntry : mp_PendingNotifications.f_Entries())
		{
			auto &PendingNotification = PendingNotificationEntry.f_Value();

			if (!_bForce && PendingNotification.m_LastAdded.f_GetTime() < 5_minutes)
				continue;

			auto &LogEntry = LogEntries.f_Insert();
			LogEntry.m_Data.m_Severity = CDistributedAppLogReporter::ELogSeverity_Info;

			TCVector<CStr> MessageParagraphs;

			auto fInsertTabbedHeading = [&](CStr const &_Heading, CStr const &_Text)
				{
					CStr Message = "{}:\n{}"_f << _Heading << _Text.f_Trim().f_Indent("    ");

					MessageParagraphs.f_Insert(fg_Move(Message));
				}
			;

			MessageParagraphs.f_Insert("New crash dump uploaded");
			if (PendingNotification.m_ExceptionInfo && *PendingNotification.m_ExceptionInfo)
				fInsertTabbedHeading("Exception Info", *PendingNotification.m_ExceptionInfo);

			fInsertTabbedHeading("Files", "{}"_f << PendingNotification.m_FileNames);

			CStr Metadata;

			if (PendingNotification.m_Metadata.m_Product)
				Metadata += "Product: {}\n"_f << *PendingNotification.m_Metadata.m_Product;

			if (PendingNotification.m_Metadata.m_Application)
				Metadata += "Application: {}\n"_f << *PendingNotification.m_Metadata.m_Application;

			if (PendingNotification.m_Metadata.m_Configuration)
				Metadata += "Configuration: {}\n"_f << *PendingNotification.m_Metadata.m_Configuration;

			if (PendingNotification.m_Metadata.m_GitBranch)
				Metadata += "GitBranch: {}\n"_f << *PendingNotification.m_Metadata.m_GitBranch;

			if (PendingNotification.m_Metadata.m_GitCommit)
				Metadata += "GitCommit: {}\n"_f << *PendingNotification.m_Metadata.m_GitCommit;

			if (PendingNotification.m_Metadata.m_Platform)
				Metadata += "Platform: {}\n"_f << *PendingNotification.m_Metadata.m_Platform;

			if (PendingNotification.m_Metadata.m_Version)
				Metadata += "Version: {}\n"_f << *PendingNotification.m_Metadata.m_Version;

			if (PendingNotification.m_Metadata.m_Tags)
				Metadata += "Tags: {vs}\n"_f << *PendingNotification.m_Metadata.m_Tags;

			fInsertTabbedHeading("Metadata", Metadata);

			LogEntry.m_Data.m_Message = CStr::fs_Join(MessageParagraphs, "\n\n");
			NotificationsToDelete[PendingNotificationEntry.f_Key()];
		}

		for (auto ToDelete : NotificationsToDelete)
			mp_PendingNotifications.f_Remove(ToDelete);

		if (mp_PendingNotifications.f_IsEmpty() && mp_PendingNotificationsTimeout)
			co_await fg_Exchange(mp_PendingNotificationsTimeout, nullptr)->f_Destroy();

		if (!LogEntries.f_IsEmpty())
		{
			if (!mp_NotificationLogReporter)
				co_await fp_Notify_OpenLogReporter();

			auto &Reporter = *mp_NotificationLogReporter;

			co_await Reporter.m_fReportEntries(fg_Move(LogEntries));
		}

		co_return {};
	}
}
