// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_AppManager.h"
#include "Malterlib_Cloud_App_AppManager_Database.h"

namespace NMib::NCloud::NAppManager
{
	using namespace NAppManagerDatabase;

	TCFuture<void> CAppManagerActor::fp_PerformDatabaseCleanup()
	{
		co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this](CDatabaseActor::CTransactionWrite _Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_return co_await fp_CleanupDatabase(fg_Move(_Transaction));
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_SetupDatabaseCleanup()
	{
		mp_RetentionDays = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("RetentionDays", 30).f_Integer();

		if (!mp_RetentionDays)
			co_return {};

		co_await fp_PerformDatabaseCleanup();

		mp_CleanupTimerSubscription = co_await fg_RegisterTimer
			(
				24_hours
				, [this]() -> TCFuture<void>
				{
					auto Result = co_await fp_PerformDatabaseCleanup().f_Wrap();

					if (!Result)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to do nightly cleanup: {}", Result.f_GetExceptionStr());

					co_return {};
				}
			)
		;

		co_return {};
	}

	TCFuture<NDatabase::CDatabaseActor::CTransactionWrite> CAppManagerActor::fp_CleanupDatabase(NDatabase::CDatabaseActor::CTransactionWrite _WriteTransaction)
	{
		auto WriteTransaction = fg_Move(_WriteTransaction);

		auto CaptureScope = co_await g_CaptureExceptions;

		auto OriginalStats = _WriteTransaction.m_Transaction.f_SizeStatistics();
		auto TargetSize = OriginalStats.m_MapSize - fg_Min(fg_Max(OriginalStats.m_MapSize / 5, 32 * OriginalStats.m_PageSize), OriginalStats.m_MapSize / 2);

		bool bDoRetention = mp_RetentionDays > 0;
		NTime::CTime OldestAllowedNotification = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(mp_RetentionDays);
		auto CurrentStats = OriginalStats;
		[[maybe_unused]] umint nNotificationsDeleted = 0;
		NTime::CTime StartTime = NTime::CTime::fs_EndOfTime();
		NTime::CTime EndTime = NTime::CTime::fs_StartOfTime();

		CRootInfoKey RootKey;
		CRootInfoValue RootValue;
		{
			auto WriteCursor = _WriteTransaction.m_Transaction.f_WriteCursor();
			if (WriteCursor.f_FindEqual(RootKey))
				RootValue = WriteCursor.f_Value<CRootInfoValue>();
		}

		RootValue.m_LastUpdateSequenceAtCleanup = mp_LastUpdateSequence;

		for (auto iNotification = _WriteTransaction.m_Transaction.f_WriteCursor(CUpdateNotificationKey::mc_Prefix); iNotification;)
		{
			CurrentStats = WriteTransaction.m_Transaction.f_SizeStatistics();
			if (CurrentStats.m_UsedBytes < TargetSize)
			{
				if (!bDoRetention)
					break;
			}

			auto Key = iNotification.f_Key<CUpdateNotificationKey>();
			auto Value = iNotification.f_Value<CUpdateNotificationValue>();

			if (Value.m_Notification.m_StartUpdateTime < OldestAllowedNotification || Value.m_UniqueKey != mp_DatabaseUniqueKey || CurrentStats.m_UsedBytes >= TargetSize)
			{
				StartTime = fg_Min(StartTime, Value.m_Notification.m_StartUpdateTime);
				EndTime = fg_Max(EndTime, Value.m_Notification.m_StartUpdateTime);

				iNotification.f_Delete();
				++nNotificationsDeleted;
			}
			else
			{
				++iNotification;
				RootValue.m_LastUpdateSequenceAtCleanup = fg_Max(RootValue.m_LastUpdateSequenceAtCleanup, Key.m_UniqueSequence);
			}
		}

		{
			auto WriteCursor = _WriteTransaction.m_Transaction.f_WriteCursor();
			WriteCursor.f_Upsert(RootKey, RootValue);
		}

		if (StartTime != NTime::CTime::fs_EndOfTime())
		{
			NTime::CTimeSpan UtcOffset;
			NTime::CSystem_Time::fs_TimeGetUTCOffset(&UtcOffset);

			[[maybe_unused]] auto UtcHourOffset = NTime::CTimeSpanConvert(UtcOffset).f_GetHours();
			[[maybe_unused]] auto UtcMinuteOffset = NTime::CTimeSpanConvert(UtcOffset).f_GetMinuteOfHour();
			[[maybe_unused]] ch8 const *pUtcSign = UtcHourOffset < 0 ? "-" : "+";
			UtcHourOffset = fg_Abs(UtcHourOffset);

			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Info
					, "Freed up {ns } bytes by deleting {} notifications spanning from {tc5} UTC{}{sj2,sf0}:{sj2,sf0} to {tc5} UTC{}{sj2,sf0}:{sj2,sf0}"
					, OriginalStats.m_UsedBytes - CurrentStats.m_UsedBytes
					, nNotificationsDeleted
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
		}

		co_return fg_Move(WriteTransaction);
	}
}
