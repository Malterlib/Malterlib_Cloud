// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_KeyManager.h"
#include "Malterlib_Cloud_KeyManagerServer.h"
#include "Malterlib_Cloud_KeyManagerServer_Internal.h"

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NConcurrency::NPrivate;
	using namespace NStr;

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_ReadDatabase()
	{
		if (m_pDatabase)
			co_return {};

		if (m_bReadingDatabase)
		{
			co_await m_OnDatabaseReadyQueue.f_Insert().f_Future();
			co_return {};
		}

		m_bReadingDatabase = true;
		auto Cleanup = g_OnScopeExit / [&]
			{
				m_bReadingDatabase = false;
			}
		;

		auto Database = co_await m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Wrap();

		if (!Database)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to read database: {}", Database.f_GetExceptionStr());

			auto Exception = Database.f_GetException();
			for (auto &Promise : m_OnDatabaseReadyQueue)
				Promise.f_SetException(Exception);
			
			m_OnDatabaseReadyQueue.f_Clear();

			co_return fg_Move(Exception);
		}

		m_pDatabase = fg_Construct(fg_Move(*Database));

		for (auto &Promise : m_OnDatabaseReadyQueue)
			Promise.f_SetResult();

		m_OnDatabaseReadyQueue.f_Clear();
		
		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys)
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		co_await Internal.f_ReadDatabase();

		DMibCheck(Internal.m_pDatabase);

		auto const& CurrentKeys = Internal.m_pDatabase->m_AvailableKeys[_KeySize];
		if (CurrentKeys.f_GetLen() >= _nKeys)
			co_return {};

		NContainer::TCVector<CSymmetricKey> GeneratedKeys;
		GeneratedKeys.f_SetLen(_nKeys - CurrentKeys.f_GetLen());

		for (auto& Key : GeneratedKeys)
		{
			Key.f_SetLen(_KeySize);
			NSys::fg_Security_GenerateHighEntropyData(Key.f_GetArray(), Key.f_GetLen());
		}

		mint nCreated = GeneratedKeys.f_GetLen();

		Internal.m_pDatabase->m_AvailableKeys[_KeySize].f_InsertFirst(fg_Move(GeneratedKeys));

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, *Internal.m_pDatabase).f_Wrap();
		if (!WriteResult)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to write database: {}", WriteResult.f_GetExceptionStr());
			co_return AppAuditor.f_Exception("Failed to write database: {}"_f << WriteResult.f_GetExceptionStr());
		}

		AppAuditor.f_Info("Pre-created {} keys of {} bytes"_f << nCreated << _KeySize);

		co_return {};
	}
}
