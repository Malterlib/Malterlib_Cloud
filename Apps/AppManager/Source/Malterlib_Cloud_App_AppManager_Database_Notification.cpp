 // Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include "Malterlib_Cloud_App_AppManager_Database.h"

#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NAppManager
{
	using namespace NAppManagerDatabase;

	TCFuture<uint64> CAppManagerActor::fp_StoreUpdateNotification(CAppManagerInterface::CUpdateNotification _Notification)
	{
		auto pCanDestroy = mp_pCanDestroy;

		auto UpdateSequence = ++mp_LastUpdateSequence;

		_Notification.m_UniqueSequence = UpdateSequence;

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [=, Notification = fg_Move(_Notification)]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);

					auto SizeStats = WriteTransaction.m_Transaction.f_SizeStatistics();
					smint BatchLimit = fg_Min(SizeStats.m_MapSize / (20 * 3), 4 * 1024 * 1024);
					auto SizeLimit = smint(SizeStats.m_MapSize)
						- fg_Min(fg_Max(smint(SizeStats.m_MapSize) / 20, BatchLimit * 3, 32 * smint(SizeStats.m_PageSize)), smint(SizeStats.m_MapSize) / 3)
					;
					smint nBytesLimit = smint(SizeLimit) - smint(SizeStats.m_UsedBytes);

					{
						smint nBytesInserted = WriteTransaction.m_Transaction.f_SizeStatistics().m_UsedBytes - SizeStats.m_UsedBytes;
						bool bFreeUpSpace = nBytesInserted >= nBytesLimit || _bCompacting;

						if (bFreeUpSpace)
							WriteTransaction = co_await fg_CallSafe(this, &CAppManagerActor::fp_CleanupDatabase, fg_Move(WriteTransaction));

						if (nBytesInserted > BatchLimit || bFreeUpSpace)
						{
							if (_bCompacting)
								co_await mp_DatabaseActor(&CDatabaseActor::f_Compact, fg_Move(WriteTransaction), 0);
							else
								co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));

							WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
							SizeStats = WriteTransaction.m_Transaction.f_SizeStatistics();
							nBytesLimit = smint(SizeLimit) - smint(SizeStats.m_UsedBytes);
						}

						CUpdateNotificationKey Key;
						Key.m_UniqueSequence = UpdateSequence;

						CUpdateNotificationValue Value;
						Value.m_UniqueKey = mp_DatabaseUniqueKey;
						Value.m_Notification = Notification;

						WriteTransaction.m_Transaction.f_Upsert(Key, Value);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Critical, "Error saving notification to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return UpdateSequence;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StoredUpdateNotificationList(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Sequence", "Update ID", "Application", "Version", "Version Time", "Start Update", "Stage", "Update Time", "Coordinated Wait", "Message");
		TableRenderer.f_SetOptions(CTableRenderHelper::EOption_Rounded | CTableRenderHelper::EOption_AvoidRowSeparators);

		{
			auto CaptureScope = co_await (g_CaptureExceptions % "Error reading notification data from database");

			auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

			for (auto iNotification = ReadTransaction.m_Transaction.f_ReadCursor(CUpdateNotificationKey::mc_Prefix); iNotification; ++iNotification)
			{
				auto Key = iNotification.f_Key<CUpdateNotificationKey>();
				auto Value = iNotification.f_Value<CUpdateNotificationValue>();

				TableRenderer.f_AddRow
					(
						"{}"_f << Key.m_UniqueSequence
						, Value.m_Notification.m_UpdateID
						, Value.m_Notification.m_Application
						, "{}"_f << Value.m_Notification.m_VersionID
						, "{tc5}"_f << Value.m_Notification.m_VersionTime
						, "{tc6}"_f << Value.m_Notification.m_StartUpdateTime
						, CAppManagerInterface::fs_UpdateStageToStr(Value.m_Notification.m_Stage)
						, "{fe1}"_f << Value.m_Notification.m_UpdateTime
						, Value.m_Notification.m_bCoordinateWait ? "true" : "false"
						, Value.m_Notification.m_Message
					)
				;
			}
		}

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CAppManagerActor::fp_CommandLine_StoredUpdateNotificationClear(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto CaptureScope = co_await (g_CaptureExceptions % "Error clearing notification data in database");

		auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);

		for (auto iNotification = WriteTransaction.m_Transaction.f_WriteCursor(CUpdateNotificationKey::mc_Prefix); iNotification;)
			iNotification.f_Delete();

		co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));

		co_return 0;
	}
}
