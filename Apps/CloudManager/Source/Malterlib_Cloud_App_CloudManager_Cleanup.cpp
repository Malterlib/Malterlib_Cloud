// Copyright © 2022 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	TCFuture<void> CCloudManagerServer::fp_PerformCleanup()
	{
		bool bForceCompact = false;
		do
		{
			auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
			WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));
			if (bForceCompact)
				co_await mp_DatabaseActor(&CDatabaseActor::f_Compact, fg_Move(WriteTransaction), 0);
			else
			{
				auto Result = co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction)).f_Wrap();
				if (!Result)
				{
					if (!bForceCompact && Result.f_GetExceptionStr().f_Find("MDB_MAP_FULL") >= 0)
					{
						DMibLogWithCategory(LogLocalStore, Warning, "Failed to commit cleanup transaction, forcing compaction of database");
						bForceCompact = true;
						continue;
					}
					co_return Result.f_GetException();
				}
			}

			auto StaleReadersRemoved = co_await mp_DatabaseActor(&CDatabaseActor::f_CheckForStaleReaders);
			if (StaleReadersRemoved)
				DMibLogWithCategory(LogLocalStore, Info, "Removed {} stale readers", StaleReadersRemoved);

			break;
		}
		while (true)
			;

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_SetupCleanup()
	{
		mp_RetentionDays = mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("RetentionDays", 365).f_Integer();

		if (!mp_RetentionDays)
			co_return {};

		co_await fp_PerformCleanup();

		mp_CleanupTimerSubscription = co_await fg_RegisterTimer
			(
				24.0 * 60.0 * 60.0
				, [this]() -> TCFuture<void>
				{
					auto Result = co_await fp_PerformCleanup().f_Wrap();

					if (!Result)
						DMibLogWithCategory(LogLocalStore, Error, "Failed to do nightly cleanup: {}", Result.f_GetExceptionStr());

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<NDatabase::CDatabaseActor::CTransactionWrite> CCloudManagerServer::fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite &&_WriteTransaction)
	{
		auto OriginalStats = _WriteTransaction.m_Transaction.f_SizeStatistics();

		auto TargetSize = OriginalStats.m_MapSize - fg_Min(fg_Max(OriginalStats.m_MapSize / 5, 32 * OriginalStats.m_PageSize), OriginalStats.m_MapSize / 2);

		auto WriteTransaction = co_await mp_AppLogStore(&CDistributedAppLogStoreLocal::f_PrepareForCleanup, fg_Move(_WriteTransaction));
		WriteTransaction = co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_PrepareForCleanup, fg_Move(WriteTransaction));

		CDistributedAppLogStoreLocal::CCleanupHelper LogHelper;
		NTime::CTime NextLogTime;
		bool bHasLog = LogHelper.f_Init(WriteTransaction, mc_pDatabasePrefixLog, NextLogTime);

		CDistributedAppSensorStoreLocal::CCleanupHelper SensorHelper;
		NTime::CTime NextSensorTime;
		bool bHasSensor = SensorHelper.f_Init(WriteTransaction, mc_pDatabasePrefixSensor, NextSensorTime);

		bool bDoRetention = mp_RetentionDays > 0;
		NTime::CTime OldestAllowedReading = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(mp_RetentionDays);

		NTime::CTime StartTime = fg_Min(NextLogTime, NextSensorTime);
		NTime::CTime EndTime = fg_Max(NextLogTime, NextSensorTime);

		if (OriginalStats.m_UsedBytes < TargetSize)
		{
			if (!bDoRetention)
				co_return fg_Move(WriteTransaction);

			if (StartTime.f_IsValid() && StartTime > OldestAllowedReading)
				co_return fg_Move(WriteTransaction);
		}

		auto CurrentStats = OriginalStats;

		mint nReadingsDeletedSensor = 0;
		mint nReadingsDeletedLog = 0;

		while (bHasLog || bHasSensor)
		{
			bool bDoLog = false;
			bool bDoSensor = false;
			if (bHasLog && bHasSensor)
			{
				if (NextLogTime.f_IsValid() && NextSensorTime.f_IsValid())
				{
					if (NextLogTime < NextSensorTime)
						bDoLog = true;
					else
						bDoSensor = true;
				}
				else if (NextLogTime.f_IsValid())
					bDoLog = true;
				else if (NextSensorTime.f_IsValid())
					bDoSensor = true;
			}
			else if (bHasLog && NextLogTime.f_IsValid())
				bDoLog = true;
			else if (bHasSensor && NextSensorTime.f_IsValid())
				bDoSensor = true;
			else
				bDoLog = true;

			if (bDoSensor)
			{
				bHasSensor = SensorHelper.f_DeleteOne(WriteTransaction, NextSensorTime);
				EndTime = NextSensorTime;
				++nReadingsDeletedSensor;
			}
			else if (bDoLog)
			{
				bHasLog = LogHelper.f_DeleteOne(WriteTransaction, NextLogTime);
				EndTime = NextLogTime;
				++nReadingsDeletedLog;
			}

			CurrentStats = WriteTransaction.m_Transaction.f_SizeStatistics();
			if (CurrentStats.m_UsedBytes < TargetSize)
			{
				if (!bDoRetention)
					break;

				if (EndTime.f_IsValid() && EndTime > OldestAllowedReading)
					break;
			}
		}

		NTime::CTimeSpan UtcOffset;
		NTime::CSystem_Time::fs_TimeGetUTCOffset(&UtcOffset);

		auto UtcHourOffset = NTime::CTimeSpanConvert(UtcOffset).f_GetHours();
		auto UtcMinuteOffset = NTime::CTimeSpanConvert(UtcOffset).f_GetMinuteOfHour();
		ch8 const *pUtcSign = UtcHourOffset < 0 ? "-" : "+";
		UtcHourOffset = fg_Abs(UtcHourOffset);

		DMibLogWithCategory
			(
				LogLocalStore
				, Info
				, "Freed up {ns } bytes by deleting {} sensor readings and {} log entries spanning from {tc5} UTC{}{sj2,sf0}:{sj2,sf0} to {tc5} UTC{}{sj2,sf0}:{sj2,sf0}"
				, OriginalStats.m_UsedBytes - CurrentStats.m_UsedBytes
				, nReadingsDeletedSensor
				, nReadingsDeletedLog
				, StartTime.f_ToLocal()
				, pUtcSign
				, UtcHourOffset
				, UtcMinuteOffset
				, EndTime.f_ToLocal()
				, pUtcSign
				, UtcHourOffset
				, UtcMinuteOffset
			)
		;

		co_return fg_Move(WriteTransaction);
	}
}
