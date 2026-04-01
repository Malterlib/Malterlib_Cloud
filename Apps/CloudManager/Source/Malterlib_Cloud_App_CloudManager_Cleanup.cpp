// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	namespace
	{
		constexpr static auto gc_YieldInterval = 50_ms;
		constexpr static auto gc_MaxRunTime = 10_seconds;
		constexpr static auto gc_LogStatsInterval = 60_seconds;
		constexpr static auto gc_FreedPagesLimit = 10000;
	}

	TCFuture<void> CCloudManagerServer::fp_PerformCleanup()
	{
		if (mp_bDoingCleanup)
			co_return DMibErrorInstance("Another cleanup is already running");

		auto OnResume = co_await f_CheckDestroyedOnResume();

		mp_bDoingCleanup = true;
		auto Cleanup = g_OnScopeExit / [&]
			{
				mp_bDoingCleanup = false;
			}
		;

		TCSharedPointer<bool> pMoreWorkNeeded = fg_Construct(true);
		TCSharedPointer<CCleanupState> pState = fg_Construct();

		while (*pMoreWorkNeeded)
		{
			co_await mp_DatabaseActor
				(
					&CDatabaseActor::f_WriteWithCompaction
					, g_ActorFunctorWeak / [this, pMoreWorkNeeded, pState](CDatabaseActor::CTransactionWrite _Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
					{
						pState->m_bForcedCompaction = _bCompacting;

						auto Result = co_await fp_CleanupDatabase(fg_Move(_Transaction), pState);

						*pMoreWorkNeeded = Result.m_bMoreWork;

						co_return fg_Move(Result.m_Transaction);
					}
				)
			;
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_SetupCleanup()
	{
		mp_RetentionDays = mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("RetentionDays", 365).f_Integer();

		if (!mp_RetentionDays)
			co_return {};

		NConcurrency::CLogError LogError("CloudManagerDatabase");
		fp_PerformCleanup() > LogError("Failed to do initial cleanup");

		mp_CleanupTimerSubscription = co_await fg_RegisterTimer
			(
				24_hours
				, [this]() mutable -> TCFuture<void>
				{
					auto Result = co_await fp_PerformCleanup().f_Wrap();

					if (!Result)
						DMibLogWithCategory(CloudManagerDatabase, Error, "Failed to do daily cleanup: {}", Result.f_GetExceptionStr());

					co_return {};
				}
			)
		;

		co_return {};
	}

	void CCloudManagerServer::CCleanupState::f_LogStartup()
	{
		if (mp_bLoggedStart)
			return;

		mp_bLoggedStart = true;
		DMibLogWithCategory(CloudManagerDatabase, Info, "Starting database cleanup{}",  m_bForcedCompaction ? " (Compaction)" : "");
	}

	void CCloudManagerServer::CCleanupState::f_Log(bool _bProgress)
	{
		auto UtcHourOffset = NTime::CTimeSpanConvert(mp_UtcOffset).f_GetHours();
		auto UtcMinuteOffset = NTime::CTimeSpanConvert(mp_UtcOffset).f_GetMinuteOfHour();
		ch8 const *pUtcSign = UtcHourOffset < 0 ? "-" : "+";
		UtcHourOffset = fg_Abs(UtcHourOffset);

		DMibLogWithCategory
			(
				CloudManagerDatabase
				, Info
				, "{} up {ns } bytes by deleting {} sensor readings and {} log entries spanning from {tc5} UTC{}{sj2,sf0}:{sj2,sf0} to {tc5} UTC{}{sj2,sf0}:{sj2,sf0}. "
				"Yielded {} times. Yielded database {} times."
				, _bProgress ? "Freeing" : "Freed"
				, mp_OriginalStats.m_UsedBytes - mp_CurrentStats.m_UsedBytes
				, mp_nReadingsDeletedSensor
				, mp_nReadingsDeletedLog
				, mp_StartTime.f_ToLocal()
				, pUtcSign
				, UtcHourOffset
				, UtcMinuteOffset
				, mp_EndTime.f_ToLocal()
				, pUtcSign
				, UtcHourOffset
				, UtcMinuteOffset
				, mp_nYields
				, mp_nDatabaseYields
			)
		;

	}

	void CCloudManagerServer::CCleanupState::f_Initialize(NDatabase::CDatabaseActor::CTransactionWrite &_WriteTransaction)
	{
		if (!mp_bInitialized)
		{
			mp_bInitialized = true;

			mp_OriginalStats = _WriteTransaction.m_Transaction.f_SizeStatistics();
			mp_CurrentStats = mp_OriginalStats;

			mp_TargetSize = mp_OriginalStats.m_MapSize - fg_Min(fg_Max(mp_OriginalStats.m_MapSize / 5, 32 * mp_OriginalStats.m_PageSize), mp_OriginalStats.m_MapSize / 2);

			NTime::CSystem_Time::fs_TimeGetUTCOffset(&mp_UtcOffset);
		}
		else
			mp_CurrentStats = _WriteTransaction.m_Transaction.f_SizeStatistics();

		mp_MaxFreedLimit = mp_CurrentStats.m_UsedBytes - gc_FreedPagesLimit * mp_OriginalStats.m_PageSize;
	}

	auto CCloudManagerServer::fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite _WriteTransaction, TCSharedPointer<CCleanupState> _pState) -> TCFuture<CCleanupDatabaseResult>
	{
		co_return co_await
			(
				g_Dispatch(_WriteTransaction.f_Checkout())
				/
				[
					AppLogStore = mp_AppLogStore
					, AppSensorStore = mp_AppSensorStore
					, pState = fg_Move(_pState)
					, WriteTransaction = fg_Move(_WriteTransaction)
					, RetentionDays = mp_RetentionDays
				]
				() mutable -> TCFuture<CCleanupDatabaseResult>
				{
					auto &State = *pState;
					State.f_Initialize(WriteTransaction);

					WriteTransaction = co_await AppLogStore(&CDistributedAppLogStoreLocal::f_PrepareForCleanup, fg_Move(WriteTransaction));
					WriteTransaction = co_await AppSensorStore(&CDistributedAppSensorStoreLocal::f_PrepareForCleanup, fg_Move(WriteTransaction));

					CDistributedAppLogStoreLocal::CCleanupHelper LogHelper;
					NTime::CTime NextLogTime;
					bool bHasLog = LogHelper.f_Init(WriteTransaction, mcp_DatabasePrefixLog, NextLogTime);

					CDistributedAppSensorStoreLocal::CCleanupHelper SensorHelper;
					NTime::CTime NextSensorTime;
					bool bHasSensor = SensorHelper.f_Init(WriteTransaction, mcp_DatabasePrefixSensor, NextSensorTime);

					bool bDoRetention = RetentionDays > 0;
					NTime::CTime OldestAllowedReading = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(RetentionDays);

					State.mp_StartTime = fg_Min(State.mp_StartTime, NextLogTime, NextSensorTime);
					State.mp_EndTime = fg_Max(State.mp_EndTime, NextLogTime, NextSensorTime);

					if (State.mp_OriginalStats.m_UsedBytes < State.mp_TargetSize)
					{
						if (!bDoRetention)
							co_return {.m_Transaction = fg_Move(WriteTransaction)};

						if (State.mp_StartTime.f_IsValid() && State.mp_StartTime > OldestAllowedReading)
							co_return {.m_Transaction = fg_Move(WriteTransaction)};
					}

					CStopwatch RuntimeStopwatch(true);
					CStopwatch YieldStopwatch(true);

					if ((bHasLog && NextLogTime.f_IsValid()) || (bHasSensor && NextSensorTime.f_IsValid()))
						State.f_LogStartup();

					auto fCheckStatsLog = [&]()
						{
							fp64 StatsTime = State.mp_StatsStopwatch.f_GetTime();
							if (StatsTime > gc_LogStatsInterval)
							{
								State.mp_StatsStopwatch.f_AddOffset(fp64(gc_LogStatsInterval) * (StatsTime / gc_LogStatsInterval).f_Floor());
								State.f_Log(true);
							}
						}
					;

					fCheckStatsLog();

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
							State.mp_EndTime = NextSensorTime;
							++State.mp_nReadingsDeletedSensor;
						}
						else if (bDoLog)
						{
							bHasLog = LogHelper.f_DeleteOne(WriteTransaction, NextLogTime);
							State.mp_EndTime = NextLogTime;
							++State.mp_nReadingsDeletedLog;
						}
						else
							break;

						State.mp_CurrentStats = WriteTransaction.m_Transaction.f_SizeStatistics();

						if (!State.m_bForcedCompaction && State.mp_CurrentStats.m_UsedBytes < State.mp_MaxFreedLimit)
						{
							// Avoid pathological performance case in database
							++State.mp_nDatabaseYields;
							fCheckStatsLog();
							co_return {.m_Transaction = fg_Move(WriteTransaction), .m_bMoreWork = true};
						}

						if (State.mp_CurrentStats.m_UsedBytes < State.mp_TargetSize)
						{
							if (!bDoRetention)
								break;

							if (State.mp_EndTime.f_IsValid() && State.mp_EndTime > OldestAllowedReading)
								break;
						}

						fp64 YieldTime = YieldStopwatch.f_GetTime();
						if (YieldTime > gc_YieldInterval)
						{
							++State.mp_nYields;
							YieldStopwatch.f_AddOffset(fp64(gc_YieldInterval) * (YieldTime / gc_YieldInterval).f_Floor());
							co_await g_Yield;

							fCheckStatsLog();

							if (!State.m_bForcedCompaction)
							{
								if (RuntimeStopwatch.f_GetTime() > gc_MaxRunTime)
								{
									++State.mp_nDatabaseYields;
									co_return {.m_Transaction = fg_Move(WriteTransaction), .m_bMoreWork = true};
								}
							}
						}
					}

					if (State.mp_nReadingsDeletedSensor || State.mp_nReadingsDeletedLog)
						State.f_Log(false);

					co_return {.m_Transaction = fg_Move(WriteTransaction)};
				}
			)
		;
	}
}
