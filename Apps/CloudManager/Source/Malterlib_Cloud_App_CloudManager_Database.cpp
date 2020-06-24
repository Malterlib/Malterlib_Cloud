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
		auto OnResume = g_OnResume / [this]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		try
		{
			auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);
			{
				auto Cursor = WriteTransaction.m_Transaction.f_WriteCursor();
				Cursor.f_Upsert(_Key, _Data);
			}
			co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", _Exception);
			co_return _Exception.f_ExceptionPointer();
		}

		co_return {};
	}

	TCFuture<void> CCloudManagerServer::fp_RemoveAppManagerData(CStr const &_HostID)
	{
		auto OnResume = g_OnResume / [this]
			{
				if (f_IsDestroyed())
					DMibError("Shutting down");
			}
		;

		try
		{
			auto WriteTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionWrite);

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

			co_await mp_DatabaseActor(&CDatabaseActor::f_CommitWriteTransaction, fg_Move(WriteTransaction));
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory(CloudManager, Critical, "Error saving app manager data to database: {}", _Exception);
			co_return _Exception.f_ExceptionPointer();
		}

		co_return {};
	}
}
