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

		// In case you copied the database from another server, or this is a database version upgrade we need to add this host.
		for (auto &Client : m_Database.m_Clients)
		{
			for (auto &Key : Client.m_Keys)
				Key.m_VerifiedOnServers[m_ThisHostID];
		}

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

		umint nKeysToAdd = _nKeys - CurrentKeys.f_GetLen();
		for (umint i = 0; i < nKeysToAdd; ++i)
		{
			CSymmetricKey ToAdd;
			ToAdd.f_SetLen(_KeySize);
			NSys::fg_Security_GenerateHighEntropyData(ToAdd.f_GetArray(), ToAdd.f_GetLen());

			GeneratedKeys[ToAdd];
		}

		umint nCreated = GeneratedKeys.f_GetLen();

		Internal.m_Database.m_AvailableKeys[_KeySize] += GeneratedKeys;

		co_await Internal.f_ForwardPreCreateKeys(Internal.m_ThisHostID, GeneratedKeys);

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		AppAuditor.f_Info("Pre-created {} keys of {} bytes"_f << nCreated << _KeySize);

		co_return {};
	}

	NConcurrency::TCFuture<uint32> CKeyManagerServer::f_RemovePreCreatedKeys(NStorage::TCOptional<uint32> _KeySize)
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		NContainer::TCSet<CSymmetricKey> RemovedKeys;
		if (_KeySize)
		{
			auto pAvailableKeys = Internal.m_Database.m_AvailableKeys.f_FindEqual(*_KeySize);
			if (pAvailableKeys)
			{
				RemovedKeys = fg_Move(*pAvailableKeys);

				Internal.m_Database.m_AvailableKeys.f_Remove(pAvailableKeys);
			}
		}
		else
		{
			for (auto &Keys : Internal.m_Database.m_AvailableKeys)
				RemovedKeys += fg_Move(Keys);

			Internal.m_Database.m_AvailableKeys.f_Clear();
		}

		if (RemovedKeys.f_IsEmpty())
			co_return 0;

		umint nRemoved = RemovedKeys.f_GetLen();

		co_await Internal.f_ForwardRemovePreCreatedKeys(Internal.m_ThisHostID, fg_Move(RemovedKeys));

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		AppAuditor.f_Info("Removed {} pre-created keys"_f << nRemoved);

		co_return nRemoved;
	}

	NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> CKeyManagerServer::f_RemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs)
	{
		return mp_pInternal->m_KeyManagerServerSyncInstance.m_pActor->f_RemoveVerifiedHosts
			(
				_HostIDs
				, NContainer::TCSet<NStr::CStr>{}
			)
		;
	}

	NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> CKeyManagerServer::f_GetAllVerifiedHosts()
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		NContainer::TCSet<NStr::CStr> Hosts;

		for (auto &Client : Internal.m_Database.m_Clients)
		{
			for (auto &Key : Client.m_Keys)
				Hosts += Key.m_VerifiedOnServers;
		}

		AppAuditor.f_Info("Returned verified host IDs");

		co_return fg_Move(Hosts);
	}

	auto CKeyManagerServer::f_GetKeys() -> NConcurrency::TCFuture<NContainer::TCVector<CKeyManagerKey>>
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		NContainer::TCVector<CKeyManagerKey> Keys;

		for (auto &Client : Internal.m_Database.m_Clients)
		{
			for (auto &Key : Client.m_Keys)
				Keys.f_Insert(CKeyManagerKey{.m_Key = CKeyManagerKeyID{.m_HostID = Client.f_GetID(), .m_KeyID = Key.f_GetID()}, .m_VerifiedOnServers = Key.m_VerifiedOnServers});
		}

		AppAuditor.f_Info("Returned all keys");

		co_return fg_Move(Keys);
	}

	NConcurrency::TCFuture<NContainer::TCMap<uint32, uint32>> CKeyManagerServer::f_GetPreCreatedKeysStats()
	{
		auto &Internal = *mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		NContainer::TCMap<uint32, uint32> KeyStats;

		for (auto &KeyEntry : Internal.m_Database.m_AvailableKeys.f_Entries())
			KeyStats[KeyEntry.f_Key()] = KeyEntry.f_Value().f_GetLen();

		AppAuditor.f_Info("Returned statistics for pre-created keys");

		co_return fg_Move(KeyStats);
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_CopyKey(CKeyManagerKeyID _FromKey, CKeyManagerKeyID _ToKey)
	{
		auto &Internal = *mp_pInternal;

		auto *pSourceClient = Internal.m_Database.m_Clients.f_FindEqual(_FromKey.m_HostID);
		if (!pSourceClient)
			co_return DMibErrorInstance("The source host was not found");

		auto *pSourceKey = pSourceClient->m_Keys.f_FindEqual(_FromKey.m_KeyID);
		if (!pSourceKey)
			co_return DMibErrorInstance("The source key was not found");

		if (!CActorDistributionManager::fs_IsValidHostID(_ToKey.m_HostID))
			co_return DMibErrorInstance("'{}' is not a valid host ID"_f << _ToKey.m_HostID);

		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> KeysToCreate;
		KeysToCreate[CKeyManagerServerSync::CHostKeyID{.m_HostID = _ToKey.m_HostID, .m_KeyID = _ToKey.m_KeyID}] = pSourceKey->m_Key;

		co_await Internal.m_KeyManagerServerSyncInstance.m_pActor->f_CreateNewKeys(fg_Move(KeysToCreate));

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::f_ForceWriteDatabase()
	{
		auto &Internal = *mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		co_return {};
	}
}
