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
		auto Database = co_await m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Wrap();

		if (!Database)
		{
			DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to read database: {}", Database.f_GetExceptionStr());

			co_return fg_Move(Database.f_GetException());
		}

		m_Database = fg_Move(*Database);

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys)
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		auto const &CurrentKeys = Internal.m_Database.m_AvailableKeys[_KeySize];
		if (CurrentKeys.f_GetLen() >= _nKeys)
			co_return {};

		NContainer::TCSet<CSymmetricKey> GeneratedKeys;

		mint nKeysToAdd = _nKeys - CurrentKeys.f_GetLen();
		for (mint i = 0; i < nKeysToAdd; ++i)
		{
			CSymmetricKey ToAdd;
			ToAdd.f_SetLen(_KeySize);
			NSys::fg_Security_GenerateHighEntropyData(ToAdd.f_GetArray(), ToAdd.f_GetLen());

			GeneratedKeys[ToAdd];
		}

		mint nCreated = GeneratedKeys.f_GetLen();

		Internal.m_Database.m_AvailableKeys[_KeySize] += GeneratedKeys;

		co_await Internal.f_ForwardPreCreateKeys(Internal.m_ThisHostID, GeneratedKeys);

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		AppAuditor.f_Info("Pre-created {} keys of {} bytes"_f << nCreated << _KeySize);

		co_return {};
	}
}
