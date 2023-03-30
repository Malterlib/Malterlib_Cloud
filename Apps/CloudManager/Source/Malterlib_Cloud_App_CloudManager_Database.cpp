 // Copyright © 2019 Nonna Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_CloudManager.h"
#include "Malterlib_Cloud_App_CloudManager_Internal.h"
#include "Malterlib_Cloud_App_CloudManager_Database.h"

namespace NMib::NCloud::NCloudManager
{
	using namespace NCloudManagerDatabase;

	CStr const &CCloudManagerServer::CAppManagerState::f_AppManagerID() const
	{
		return TCMap<CStr, CAppManagerState>::fs_GetKey(*this);
	}

	CAppManagerKey CCloudManagerServer::CAppManagerState::f_DatabaseKey() const
	{
		return CAppManagerKey{CAppManagerKey::mc_Prefix, f_AppManagerID()};
	}

	TCFuture<void> CCloudManagerServer::fp_SaveAppManagerData(NCloudManagerDatabase::CAppManagerKey _Key, NCloudManagerDatabase::CAppManagerValue _Data)
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

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [Key = fg_Move(_Key), Data = fg_Move(_Data)]
				(CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureExceptions;

					auto WriteTransaction = fg_Move(_Transaction);

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

		auto Result = co_await mp_DatabaseActor
			(
				&CDatabaseActor::f_WriteWithCompaction
				, g_ActorFunctorWeak / [_HostID](CDatabaseActor::CTransactionWrite &&_Transaction, bool _bCompacting) -> TCFuture<CDatabaseActor::CTransactionWrite>
				{
					co_await ECoroutineFlag_CaptureExceptions;

					auto WriteTransaction = fg_Move(_Transaction);

					bool bFoundAppManager = false;
					for (auto AppManagerCursor = WriteTransaction.m_Transaction.f_WriteCursor(CAppManagerKey::mc_Prefix, _HostID); AppManagerCursor;)
					{
						AppManagerCursor.f_Delete();
						bFoundAppManager = true;
					}

					if (!bFoundAppManager)
						co_return DMibErrorInstance("App manager does not exist");

					for (auto ApplicationCursor = WriteTransaction.m_Transaction.f_WriteCursor(CApplicationKey::mc_Prefix, _HostID); ApplicationCursor;)
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

		co_return {};
	}
}
