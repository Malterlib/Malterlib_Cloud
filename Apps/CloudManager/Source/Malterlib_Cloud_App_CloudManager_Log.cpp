// Copyright © 2022 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/CommandLine/AnsiEncoding>

namespace NMib::NCloud::NCloudManager
{
	TCFuture<void> CCloudManagerServer::fp_SetupLogStore()
	{
		mp_AppLogStore = fg_Construct(mp_AppState.m_DistributionManager, mp_AppState.m_TrustManager);
		co_await mp_AppLogStore
			(
				&CDistributedAppLogStoreLocal::f_StartWithDatabase
				, fg_TempCopy(mp_DatabaseActor)
				, mc_pDatabasePrefixLog
				, g_ActorFunctor / [this](NDatabase::CDatabaseActor::CTransactionWrite &&_WriteTransaction) -> TCFuture<NDatabase::CDatabaseActor::CTransactionWrite>
				{
					co_return co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(_WriteTransaction));
				}
			)
		;

		co_return {};
	}

	auto CCloudManagerServer::CDistributedAppLogReporterImplementation::f_OpenLogReporter(CLogInfo &&_LogInfo) -> TCFuture<CLogReporter>
	{
		auto pThis = m_pThis;
		auto OnResume = co_await fg_OnResume
			(
				[pThis]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto &HostInfo = Auditor.f_HostInfo();
		auto ReportingHostID = HostInfo.f_GetRealHostID();

		NContainer::TCVector<CStr> Permissions;

		CDistributedAppLogReporter::CLogInfo LogInfo = fg_Move(_LogInfo);
		if (!LogInfo.m_HostID)
		{
			LogInfo.m_HostID = ReportingHostID;
			LogInfo.m_HostName = HostInfo.f_GetHostInfo().m_FriendlyName;
		}

		if (LogInfo.m_HostID == ReportingHostID)
			Permissions.f_Insert("CloudManager/ReportLogEntries");
		else
		{
			Permissions.f_Insert("CloudManager/ReportLogEntriesOnBehalfOf/All");
			Permissions.f_Insert("CloudManager/ReportLogEntriesOnBehalfOf/{}"_f << LogInfo.m_HostID);
		}

		if (!co_await pThis->mp_Permissions.f_HasPermission("Open log reporter", Permissions, Auditor.f_HostInfo()))
			co_return Auditor.f_AccessDenied("(Open log reporter)", Permissions);

		auto Key = LogInfo.f_Key();

		auto Reporter = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_OpenLogReporter, fg_Move(LogInfo));

		Auditor.f_Info("Open log reporter ({})"_f << Key);

		co_return fg_Move(Reporter);
	}

	TCVector<CStr> CCloudManagerServer::fsp_LogReadPermissions()
	{
		return {"CloudManager/ReadLogs", "CloudManager/ReadAll"};
	}

	auto CCloudManagerServer::CDistributedAppLogReaderImplementation::f_GetLogs(CDistributedAppLogReader_LogFilter &&_Filter, uint32 _BatchSize)
		-> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>> Logs;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await fg_OnResume
				(
					[pThis]() -> CExceptionPointer
					{
						if (pThis->f_IsDestroyed())
							return DMibErrorInstance("Shutting down");
						return {};
					}
				)
			;

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_LogReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get logs", Permissions))
				co_return Auditor.f_AccessDenied("(Get logs)", Permissions);

			Logs = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_GetLogs, fg_Move(_Filter), _BatchSize);

			Auditor.f_Info("Get logs");
		}

		co_return fg_Move(Logs);
	}

	auto CCloudManagerServer::CDistributedAppLogReaderImplementation::f_GetLogEntries(CDistributedAppLogReader_LogEntryFilter &&_Filter, uint32 _BatchSize)
		-> TCFuture<TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>>>
	{
		TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>> LogEntriesGenerator;
		{
			auto pThis = m_pThis;
			auto OnResume = co_await fg_OnResume
				(
					[pThis]() -> CExceptionPointer
					{
						if (pThis->f_IsDestroyed())
							return DMibErrorInstance("Shutting down");
						return {};
					}
				)
			;

			auto Auditor = pThis->mp_AppState.f_Auditor();

			auto Permissions = fsp_LogReadPermissions();

			if (!co_await pThis->mp_Permissions.f_HasPermission("Get log entries", Permissions))
				co_return Auditor.f_AccessDenied("(Get log entries)", Permissions);

			LogEntriesGenerator = co_await pThis->mp_AppLogStore(&CDistributedAppLogStoreLocal::f_GetLogEntries, fg_Move(_Filter), _BatchSize);

			Auditor.f_Info("Get log entries");
		}

		co_return fg_Move(LogEntriesGenerator);
	}
}
