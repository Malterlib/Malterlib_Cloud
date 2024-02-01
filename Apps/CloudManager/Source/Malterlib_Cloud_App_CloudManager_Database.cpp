 // Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/CommandLine/TableRenderer>
#include <Mib/Encoding/ToJson>

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

	CEJSONSorted CCloudManagerGlobalStateKey::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Prefix"] = fg_ToJson(m_Prefix);
		return Return;
	}

	CEJSONSorted CCloudManagerGlobalStateValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["SensorProblemsSlackThread"] = fg_ToJson(m_SensorProblemsSlackThread);
		Return["LastSeenLogTimestamp"] = fg_ToJson(m_LastSeenLogTimestamp);
		return Return;
	}

	CEJSONSorted CAppManagerKey::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Prefix"] = fg_ToJson(m_Prefix);
		Return["HostID"] = fg_ToJson(m_HostID);
		return Return;
	}

	CEJSONSorted CAppManagerValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Info"] = fg_ToJson(m_Info);
		Return["LastSeen"] = fg_ToJson(m_LastSeen);
		Return["LastConnectionError"] = fg_ToJson(m_LastConnectionError);
		Return["LastConnectionErrorTime"] = fg_ToJson(m_LastConnectionErrorTime);
		Return["OtherErrors"] = fg_ToJson(m_OtherErrors);
		Return["LastSeenUpdateNotificationSequence"] = fg_ToJson(m_LastSeenUpdateNotificationSequence);
		Return["bActive"] = fg_ToJson(m_bActive);
		Return["PauseReportingFor"] = fg_ToJson(m_PauseReportingFor);
		return Return;
	}

	CEJSONSorted CApplicationKey::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Prefix"] = fg_ToJson(m_Prefix);
		Return["AppManagerHostID"] = fg_ToJson(m_AppManagerHostID);
		Return["Application"] = fg_ToJson(m_Application);
		return Return;
	}

	CEJSONSorted CApplicationValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["ApplicationInfo"] = fg_ToJson(m_ApplicationInfo);
		return Return;
	}

	CEJSONSorted CApplicationUpdateStateStage::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Time"] = fg_ToJson(m_Time);
		return Return;
	}

	CEJSONSorted CApplicationUpdateStateValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["LastUpdateID"] = fg_ToJson(m_LastUpdateID);
		Return["SlackTimestamps"] = fg_ToJson(m_SlackTimestamps);
		Return["Stages"] = fg_ToJson(m_Stages);
		Return["LastNotification"] = fg_ToJson(m_LastNotification);
		Return["LastUpdateSequence"] = fg_ToJson(m_LastUpdateSequence);
		Return["bDeferred"] = fg_ToJson(m_bDeferred);
		return Return;
	}

	CEJSONSorted CSensorNotificationStateNotificationStatus::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Severity"] = fg_ToJson(m_Severity);
		Return["Message"] = fg_ToJson(m_Message);
		return Return;
	}

	CEJSONSorted CSensorNotificationStateNotification::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Status"] = fg_ToJson(m_Status);
		Return["OutdatedStatus"] = fg_ToJson(m_OutdatedStatus);
		Return["CriticalityStatus"] = fg_ToJson(m_CriticalityStatus);
		return Return;
	}

	CEJSONSorted CSensorNotificationStateValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["LastNotification"] = fg_ToJson(m_LastNotification);
		Return["TimeInProblemState"] = fg_ToJson(m_TimeInProblemState);
		Return["bInProblemState"] = fg_ToJson(m_bInProblemState);
		Return["bInProblemStateForReporting"] = fg_ToJson(m_bInProblemStateForReporting);
		Return["bSentAlert"] = fg_ToJson(m_bSentAlert);
		return Return;
	}

	CEJSONSorted CExpectedOsVersionKey::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["Prefix"] = fg_ToJson(m_Prefix);
		Return["OsName"] = fg_ToJson(m_OsName);
		Return["CurrentVersion"] = fg_ToJson(m_CurrentVersion);
		return Return;
	}

	CEJSONSorted CExpectedOsVersionValue::f_ToJson() const
	{
		CEJSONSorted Return;
		Return["ExpectedVersionRange"] = fg_ToJson(m_ExpectedVersionRange);
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
						WriteTransaction = fg_Move((co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

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

		co_await mp_AppSensorStore
			(
				&CDistributedAppSensorStoreLocal::f_SeenHosts
				, TCMap<CStr, CDistributedAppSensorStoreLocal::CSeenHost>{{_Key.m_HostID, {_Data.m_LastSeen, _Data.m_PauseReportingFor}}}
			)
		;
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
						WriteTransaction = fg_Move((co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

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
		auto OnResume = co_await f_CheckDestroyedOnResume();

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
						WriteTransaction = fg_Move((co_await self(&CCloudManagerServer::fp_CleanupDatabase, fg_Move(WriteTransaction), fg_Construct())).m_Transaction);

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

	namespace
	{
		struct CTableDumper
		{
			template <typename tf_CKey, typename tf_CValue, typename ...tfp_CPrefix>
			void f_DumpTable(tfp_CPrefix && ...p_Prefix)
			{
				if (m_FilterPrefix && tf_CKey::mc_Prefix != m_FilterPrefix)
					return;

				for (auto iState = m_ReadTransaction.f_ReadCursor(p_Prefix...); iState; ++iState)
				{
					auto Key = iState.template f_Key<tf_CKey>();
					auto Value = iState.template f_Value<tf_CValue>();

					m_TableRenderer.f_AddRow
						(
							"{}"_f << tf_CKey::mc_Prefix
							, Key.f_ToJson().f_ToString().f_Trim()
							, Value.f_ToJson().f_ToString().f_Trim()
						)
					;
				}
			}

			CTableRenderHelper &m_TableRenderer;
			CDatabaseSubReadTransaction &m_ReadTransaction;
			CStr const &m_FilterPrefix;
		};
	}

	TCFuture<void> CCloudManagerServer::f_DumpDatabaseEntries(TCSharedPointer<CCommandLineControl> const &_pCommandLine, CStr const &_Prefix)
	{
		auto CaptureScope = co_await (g_CaptureExceptions % "Error dumping database");

		auto TableRenderer = _pCommandLine->f_TableRenderer();

		CTableRenderHelper::CColumnHelper Columns(0);

		Columns.f_AddHeading("Prefix", 0);
		Columns.f_AddHeading("Key", 0);
		Columns.f_AddHeading("Value", 0);

		TableRenderer.f_AddHeadings(&Columns);

		auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

		CTableDumper Dumper{.m_TableRenderer = TableRenderer, .m_ReadTransaction = ReadTransaction.m_Transaction, .m_FilterPrefix = _Prefix};

		Dumper.f_DumpTable<CCloudManagerGlobalStateKey, CCloudManagerGlobalStateValue>(CCloudManagerGlobalStateKey::mc_Prefix);
		Dumper.f_DumpTable<CAppManagerKey, CAppManagerValue>(CAppManagerKey::mc_Prefix);
		Dumper.f_DumpTable<CApplicationKey, CApplicationValue>(CApplicationKey::mc_Prefix);
		Dumper.f_DumpTable<CApplicationUpdateStateKey, CApplicationUpdateStateValue>(CApplicationUpdateStateKey::mc_Prefix);
		Dumper.f_DumpTable<CSensorNotificationStateKey, CSensorNotificationStateValue>(CStr(), CSensorNotificationStateKey::mc_Prefix);
		Dumper.f_DumpTable<CExpectedOsVersionKey, CExpectedOsVersionValue>(CExpectedOsVersionKey::mc_Prefix);

		TableRenderer.f_Output();

		co_return {};
	}
}
