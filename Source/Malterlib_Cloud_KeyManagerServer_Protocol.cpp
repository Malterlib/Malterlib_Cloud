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

	NConcurrency::TCFuture<CSymmetricKey> CKeyManagerServer::CInternal::CKeyManagerImplementation::f_RequestKey(CStr const &_Identifier, uint32 _KeySize)
	{
		auto &Internal = *m_pThis->mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		auto HostID = NConcurrency::CActorDistributionManager::fs_GetCallingHostInfo().f_GetRealHostID();

		co_await Internal.f_ReadDatabase();

		DMibCheck(Internal.m_pDatabase);

		auto &Client = Internal.m_pDatabase->m_Clients[HostID];
		auto pKey = Client.m_Keys.f_FindEqual(_Identifier);
		if (!pKey)
		{
			pKey = &Client.m_Keys[_Identifier];

			auto pAvailableKeys = Internal.m_pDatabase->m_AvailableKeys.f_FindEqual(_KeySize);
			if (!pAvailableKeys)
			{
				pKey->f_SetLen(_KeySize);
				NSys::fg_Security_GenerateHighEntropyData(pKey->f_GetArray(), pKey->f_GetLen());
				AppAuditor.f_Info("Created new key of {} bytes for ID '{}'"_f << _KeySize << _Identifier);
			}
			else
			{
				*pKey = pAvailableKeys->f_GetLast();
				if (pKey->f_GetLen() != _KeySize)
				{
					CStr Error = "Requested key size mismatch with pre-created key. Requested: {}, Pre-created: {}"_f << _KeySize << pKey->f_GetLen();
					DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "{}", Error);
					co_return AppAuditor.f_Exception(Error);
				}

				AppAuditor.f_Info("Used pre-created key of {} bytes for ID '{}'"_f << _KeySize << _Identifier);

				pAvailableKeys->f_Remove(pAvailableKeys->f_GetLen() - 1);
				if (pAvailableKeys->f_IsEmpty())
					Internal.m_pDatabase->m_AvailableKeys.f_Remove(_KeySize);
			}

			auto Key = *pKey;

			auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, *Internal.m_pDatabase).f_Wrap();
			if (!WriteResult)
			{
				DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Error, "Failed to write database: {}", WriteResult.f_GetExceptionStr());
				co_return AppAuditor.f_Exception("Failed to write database: {}"_f <<WriteResult.f_GetExceptionStr());
			}

			co_return fg_Move(Key);
		}
		else
		{
			if (pKey->f_GetLen() != _KeySize)
				co_return AppAuditor.f_Exception("Saved key has different size {} from requested size {}"_f << pKey->f_GetLen() << _KeySize);

			AppAuditor.f_Info("Returned key of {} bytes with ID '{}'"_f << _KeySize << _Identifier);

			co_return *pKey;
		}
	}
}
