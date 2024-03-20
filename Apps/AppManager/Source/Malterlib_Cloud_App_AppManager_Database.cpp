 // Copyright © 2023 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include "Malterlib_Cloud_App_AppManager_Database.h"

#include <Mib/CommandLine/TableRenderer>

namespace NMib::NCloud::NAppManagerDatabase
{
	constexpr uint64 gc_MaxDatabaseSize = constant_uint64(1) * 1024 * 1024 * 1024;

	constexpr CStr CUpdateNotificationKey::mc_Prefix = gc_Str<"UpdateNot">;
	constexpr CStr CRootInfoKey::mc_Prefix = gc_Str<"RootInfo">;
}

namespace NMib::NCloud::NAppManager
{
	using namespace NAppManagerDatabase;

	TCFuture<void> CAppManagerActor::fp_SetupDatabase()
	{
		mp_DatabaseActor = fg_Construct();
		auto MaxDatabaseSize = mp_State.m_ConfigDatabase.m_Data.f_GetMemberValue("MaxDatabaseSize", gc_MaxDatabaseSize).f_Integer();
		co_await
			(
				mp_DatabaseActor
				(
					&CDatabaseActor::f_OpenDatabase
					, mp_State.m_RootDirectory / "AppManagerDatabase"
					, MaxDatabaseSize
				)
				% "Failed to open database"
			)
		;
		auto Stats = co_await (mp_DatabaseActor(&CDatabaseActor::f_GetAggregateStatistics));
		auto TotalSizeUsed = Stats.f_GetTotalUsedSize();
		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Database uses {fe2}% of allotted space ({ns } / {ns } bytes). {ns } records."
				, fp64(TotalSizeUsed) / fp64(MaxDatabaseSize) * 100.0
				, TotalSizeUsed
				, MaxDatabaseSize
				, Stats.m_nDataItems
			)
		;

		co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this](CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);

					if (_bCompacting)
						WriteTransaction = co_await fp_CleanupDatabase(fg_Move(WriteTransaction));

					auto WriteCursor = WriteTransaction.m_Transaction.f_WriteCursor();

					CRootInfoKey Key;

					CRootInfoValue Value;
					if (WriteCursor.f_FindEqual(Key))
						Value = WriteCursor.f_Value<CRootInfoValue>();

					if (!Value.m_UniqueKey)
						Value.m_UniqueKey = fg_RandomID();

					WriteCursor.f_Upsert(Key, Value);

					mp_DatabaseUniqueKey = Value.m_UniqueKey;
					mp_LastUpdateSequence = Value.m_LastUpdateSequenceAtCleanup;

					{
						auto ReadCursor = WriteTransaction.m_Transaction.f_ReadCursor(CUpdateNotificationKey::mc_Prefix);
						if (ReadCursor.f_Last())
							mp_LastUpdateSequence = fg_Max(mp_LastUpdateSequence, ReadCursor.f_Key<CUpdateNotificationKey>().m_UniqueSequence);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
		;

		co_return {};
	}
}
