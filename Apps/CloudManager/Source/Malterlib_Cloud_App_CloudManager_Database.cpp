 // Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManagerDatabase
{
	constexpr CStr CCloudManagerGlobalStateKey::mc_Prefix = gc_Str<"Global">;
	constexpr CStr CAppManagerKey::mc_Prefix = gc_Str<"AppMgr">;
	constexpr CStr CApplicationKey::mc_Prefix = gc_Str<"App">;
	constexpr CStr CApplicationUpdateStateKey::mc_Prefix = gc_Str<"AppUpSt">;
	constexpr CStr CSensorNotificationStateKey::mc_Prefix = gc_Str<"SensNoSt">;
	constexpr CStr CExpectedOsVersionKey::mc_Prefix = gc_Str<"ExpectedOV">;


	auto CSensorNotificationStateKey::f_SensorKey() const & -> CSensorKey
	{
		CSensorKey Return = *this;

		Return.m_Prefix = CSensorKey::mc_Prefix;

		return Return;
	}

	auto CSensorNotificationStateKey::f_SensorKey() && -> CSensorKey
	{
		CSensorKey Return = fg_Move(*this);

		Return.m_Prefix = CSensorKey::mc_Prefix;

		return Return;
	}
}

namespace NMib::NCloud::NCloudManager
{
	constinit uint64 g_MaxDatabaseSize = constant_uint64(128) * 1024 * 1024 * 1024;

	using namespace NCloudManagerDatabase;

	CStr const &CCloudManagerServer::CAppManagerState::f_AppManagerID() const
	{
		return TCMap<CStr, CAppManagerState>::fs_GetKey(*this);
	}

	CAppManagerKey CCloudManagerServer::CAppManagerState::f_DatabaseKey() const
	{
		return CAppManagerKey{.m_HostID = f_AppManagerID()};
	}

	TCFuture<void> CCloudManagerServer::fp_SetupDatabase()
	{
		mp_DatabaseActor = fg_Construct(fg_Construct(), "Cloud manager database");
		auto MaxDatabaseSize = mp_AppState.m_ConfigDatabase.m_Data.f_GetMemberValue("MaxDatabaseSize", g_MaxDatabaseSize).f_Integer();
		co_await
			(
				mp_DatabaseActor
				(
					&CDatabaseActor::f_OpenDatabase
					, mp_AppState.m_RootDirectory / "CloudManagerDatabase"
					, MaxDatabaseSize
				)
				% "Failed to open database"
			)
		;
		auto Stats = co_await (mp_DatabaseActor(&CDatabaseActor::f_GetAggregateStatistics));
		auto TotalSizeUsed = Stats.f_GetTotalUsedSize();
		DMibLogWithCategory
			(
				CloudManager
				, Info
				, "Database uses {fe2}% of allotted space ({ns } / {ns } bytes). {ns } records."
				, fp64(TotalSizeUsed) / fp64(MaxDatabaseSize) * 100.0
				, TotalSizeUsed
				, MaxDatabaseSize
				, Stats.m_nDataItems
			)
		;

		{
			auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);
			auto ReadCursor = ReadTransaction.m_Transaction.f_ReadCursor();

			CCloudManagerGlobalStateKey Key;

			if (ReadCursor.f_FindEqual(Key))
				mp_GlobalState = ReadCursor.f_Value<CCloudManagerGlobalStateValue>();
		}

		co_return {};
	}

	TCFuture<CDatabaseActor::CTransactionWrite> CCloudManagerServer::fp_SaveGlobalState(CDatabaseActor::CTransactionWrite &&_Transaction)
	{
		co_await ECoroutineFlag_CaptureMalterlibExceptions;

		auto WriteTransaction = fg_Move(_Transaction);
		auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();

		Cursor.f_Upsert(CCloudManagerGlobalStateKey(), mp_GlobalState);

		co_return fg_Move(WriteTransaction);
	}

	TCFuture<void> CCloudManagerServer::fp_SaveGlobalStateWithoutTransaction()
	{
		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this](CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;

					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					WriteTransaction = co_await fp_SaveGlobalState(fg_Move(WriteTransaction));

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
			DMibLogWithCategory(CloudManager, Critical, "Error saving global state to database: {}", Result.f_GetExceptionStr());

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_SaveAppManagerData(NCloudManagerDatabase::CAppManagerKey _Key, NCloudManagerDatabase::CAppManagerValue _Data)
	{
		auto OnResume = co_await fg_OnResume
			(
				[this]() -> CExceptionPointer
				{
					if (!mp_AppSensorStore || !mp_AppLogStore || !mp_DatabaseActor)
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_SeenHosts, TCMap<CStr, CTime>{{_Key.m_HostID, _Data.m_LastSeen}});
		co_await mp_AppLogStore(&CDistributedAppLogStoreLocal::f_SeenHosts, TCMap<CStr, CTime>{{_Key.m_HostID, _Data.m_LastSeen}});

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this, Key = fg_Move(_Key), Data = fg_Move(_Data)]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;
					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					{
						auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();
						Cursor.f_Upsert(Key, Data);
					}

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_RemoveAppManagerData(CStr const &_HostID)
	{
		auto OnResume = co_await fg_OnResume
			(
				[this]() -> CExceptionPointer
				{
					if (f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		TCSharedPointer<TCSet<CStr>> pRemovedHostIDs = fg_Construct();
		(*pRemovedHostIDs)[_HostID];

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [this, _HostID, pRemovedHostIDs](CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureMalterlibExceptions;
				
					auto WriteTransaction = fg_Move(_Transaction);
					if (_bCompacting)
						WriteTransaction = co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction));

					bool bFoundAppManager = false;
					for (auto AppManagerCursor = WriteTransaction.m_Transaction.f_WriteCursor(CAppManagerKey::mc_Prefix, _HostID); AppManagerCursor;)
					{
						AppManagerCursor.f_Delete();
						bFoundAppManager = true;
					}

					if (!bFoundAppManager)
						co_return DMibErrorInstance("App manager does not exist");

					for (auto ApplicationCursor = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationKey::mc_Prefix, _HostID); ApplicationCursor;)
					{
						auto Application = ApplicationCursor.f_Value<CApplicationValue>();
						if (Application.m_ApplicationInfo.m_HostID)
							(*pRemovedHostIDs)[Application.m_ApplicationInfo.m_HostID];
						ApplicationCursor.f_Delete();
					}

					for (auto ApplicationCursor = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationUpdateStateKey::mc_Prefix, _HostID); ApplicationCursor;)
						ApplicationCursor.f_Delete();

					co_return fg_Move(WriteTransaction);
				}
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", Result.f_GetExceptionStr());
			co_return Result.f_GetException();
		}

		co_await mp_AppSensorStore(&CDistributedAppSensorStoreLocal::f_RemoveHosts, *pRemovedHostIDs);
		co_await mp_AppLogStore(&CDistributedAppLogStoreLocal::f_RemoveHosts, *pRemovedHostIDs);

		co_return {};
	}
}
