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

	CKeyManagerServer::CInternal::CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig &&_Config)
		: m_pThis(_pThis)
		, m_Config(fg_Move(_Config))
		, m_pDatabase(nullptr)
	{
		
	}
		
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
	
	CKeyManagerServer::CKeyManagerServer(CKeyManagerServerConfig &&_Config)
		: mp_pInternal(fg_Construct(this, fg_Move(_Config)))
	{
	}
	
	CKeyManagerServer::~CKeyManagerServer()
	{
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_Init()
	{
		auto &Internal = *mp_pInternal;
		Internal.m_DistributionManager = co_await Internal.m_Config.m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager);

		co_await Internal.m_KeyManagerInstance.f_Publish<CKeyManager>(Internal.m_DistributionManager, this, CKeyManager::mc_pDefaultNamespace);

		co_return {};
	}

	TCFuture<void> CKeyManagerServer::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		CLogError LogError("Mib/Cloud/KeyManagerServer");

		co_await Internal.m_KeyManagerInstance.f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy key manager instance");

		co_return {};
	}
}
