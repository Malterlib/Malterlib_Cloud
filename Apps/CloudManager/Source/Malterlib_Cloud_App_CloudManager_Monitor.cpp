// Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/CommandLine/AnsiEncoding>

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	TCFuture<void> CCloudManagerServer::fp_UpdateAppManagerState()
	{
		auto OnResume = g_OnResume / [this]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		TCActorResultMap<CStr, CActorDistributionManager::CHostState> HostStateResults;
		for (auto &AppManager : mp_AppManagers)
		{
			CStr const &AppManagerID = mp_AppManagers.fs_GetKey(AppManager);

			mp_AppState.m_DistributionManager(&CActorDistributionManager::f_GetHostState, AppManagerID) > HostStateResults.f_AddResult(AppManagerID);
		}

		try
		{
			auto HostStates = co_await HostStateResults.f_GetResults();

			auto Now = CTime::fs_NowUTC();

			TCSet<CStr> ToUpdateAppManagers;
			TCSet<CStr> ToClearAppManagers;

			{
				auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

				TCSet<CStr> SeenAppManagers;

				for (auto &State : HostStates)
				{
					if (!State)
						continue;

					CStr const &AppManagerID = HostStates.fs_GetKey(State);

					auto *pAppManager = mp_AppManagers.f_FindEqual(AppManagerID);
					if (!pAppManager)
						continue;

					SeenAppManagers[AppManagerID];

					bool bDataUpdated = false;

					if (State->m_LastConnectionError != pAppManager->m_Data.m_LastConnectionError)
					{
						pAppManager->m_Data.m_LastConnectionError = State->m_LastConnectionError;
						bDataUpdated = true;
					}

					if (State->m_LastConnectionErrorTime != pAppManager->m_Data.m_LastConnectionErrorTime)
					{
						pAppManager->m_Data.m_LastConnectionErrorTime = State->m_LastConnectionErrorTime;
						bDataUpdated = true;
					}

					if (State->m_bActive)
					{
						pAppManager->m_Data.m_LastSeen = Now;
						pAppManager->m_Data.m_bActive = true;
						bDataUpdated = true;
					}
					else if (pAppManager->m_Data.m_bActive)
					{
						pAppManager->m_Data.m_bActive = false;
						bDataUpdated = true;
					}

					if (bDataUpdated)
						ToUpdateAppManagers[AppManagerID];
				}

				for (auto AppManagers = ReadTransaction.m_Transaction.f_ReadCursor(CAppManagerKey::mc_Prefix); AppManagers; ++AppManagers)
				{
					auto Key = AppManagers.f_Key<CAppManagerKey>();
					if (SeenAppManagers.f_Exists(Key.m_HostID))
						continue;
					auto Data = AppManagers.template f_Value<CAppManagerValue>();
					if (Data.m_bActive)
						ToClearAppManagers[Key.m_HostID];
				}
			}

			if (!ToUpdateAppManagers.f_IsEmpty() || !ToClearAppManagers.f_IsEmpty())
			{
				auto Result = co_await mp_DatabaseActor
					(
						&CDatabaseActor::f_WriteWithCompaction
						, g_ActorFunctorWeak / [this, ToUpdateAppManagers = fg_Move(ToUpdateAppManagers), ToClearAppManagers = fg_Move(ToClearAppManagers)]
						(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
						{
							co_await ECoroutineFlag_CaptureExceptions;

							auto WriteTransaction = fg_Move(_Transaction);
							{
								auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();

								for (auto &AppManager : mp_AppManagers)
								{
									if (!ToUpdateAppManagers.f_IsEmpty() && !ToUpdateAppManagers.f_Exists(AppManager.f_AppManagerID()))
										continue;

									Cursor.f_Upsert(AppManager.f_DatabaseKey(), AppManager.m_Data);
								}

								for (auto &ToClear : ToClearAppManagers)
								{
									if (!Cursor.f_FindEqual(CAppManagerKey{CAppManagerKey::mc_Prefix, ToClear}))
										continue;

									auto Data = Cursor.f_Value<CAppManagerValue>();
									if (Data.m_bActive)
									{
										Data.m_bActive = false;
										Data.m_LastConnectionErrorTime = CTime::fs_NowUTC();
										Data.m_LastConnectionError = "Missing";
										Cursor.f_SetValue(Data);
									}
								}
							}
							co_return fg_Move(WriteTransaction);
						}
					)
					.f_Wrap()
				;

				if (!Result)
				{
					DMibLogWithCategory(CloudManager, Critical, "Error saving app managers to database: {}", Result.f_GetExceptionStr());
					co_return Result.f_GetException();
				}
			}
		}
		catch (CException const &_Exception)
		{
			co_return _Exception.f_ExceptionPointer();
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_SetupMonitor()
	{
		mp_MonitorTimerSubscription = co_await fg_RegisterTimer
			(
			 	60.0
			 	, [this]() -> TCFuture<void>
			 	{
					if (f_IsDestroyed())
						co_return {};

					try
					{
						co_await self(&CCloudManagerServer::fp_UpdateAppManagerState);
					}
					catch (CException const &_Exception)
					{
						[[maybe_unused]] auto &Exception = _Exception;
						DMibLogWithCategory(CloudManager, Critical, "Failed to monitor state: {}", Exception);
					}

					co_return {};
				}
			)
		;

		co_return {};
	}
}
