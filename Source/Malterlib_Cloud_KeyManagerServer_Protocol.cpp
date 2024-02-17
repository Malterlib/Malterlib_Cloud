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

	NConcurrency::TCFuture<NStorage::TCOptional<CSymmetricKey>> CKeyManagerServer::CInternal::f_TryUseAvailableKey
		(
			CStr _Identifier
			, uint32 _KeySize
			, NConcurrency::CDistributedAppAuditor _AppAuditor
		)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		NContainer::TCSet<CSymmetricKey> TriedAvailableKeys;
		while (true)
		{
			NContainer::TCSet<CSymmetricKey> *pAvailableKeys;
			auto OnResume = co_await fg_OnResume
				(
					[&]() -> NException::CExceptionPointer
					{
						pAvailableKeys = m_Database.m_AvailableKeys.f_FindEqual(_KeySize);
						return nullptr;
					}
				)
			;

			if (!pAvailableKeys || pAvailableKeys->f_IsEmpty())
				break;

			CSymmetricKey TryKey;
			for (auto &AvailableKey : *pAvailableKeys)
			{
				if (m_TryingToUseAvailableKeys.f_FindEqual(AvailableKey))
					continue;

				if (TriedAvailableKeys(AvailableKey).f_WasCreated())
				{
					TryKey = AvailableKey;
					break;
				}
			}

			if (TryKey.f_IsEmpty())
				break;

			m_TryingToUseAvailableKeys[TryKey] = m_ThisHostID;
			auto Cleanup = g_OnScopeExit / [&]
				{
					auto *pTryUse = m_TryingToUseAvailableKeys.f_FindEqual(TryKey);
					if (pTryUse && *pTryUse == m_ThisHostID)
						m_TryingToUseAvailableKeys.f_Remove(pTryUse);
				}
			;

			auto UseAvailableKey = co_await f_UseAvailableKey(TryKey);
			if (!UseAvailableKey)
				continue;

			if (!pAvailableKeys)
				break;

			auto *pUsedKey = pAvailableKeys->f_FindEqual(TryKey);
			if (!pUsedKey)
				continue;

			if (TryKey.f_GetLen() != _KeySize)
			{
				CStr Error = "Requested key size mismatch with pre-created key. Requested: {}, Pre-created: {}"_f << _KeySize << TryKey.f_GetLen();
				DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Critical, "{}", Error);
				co_return _AppAuditor.f_Exception(Error);
			}

			pAvailableKeys->f_Remove(pUsedKey);
			mint KeysLeft = pAvailableKeys->f_GetLen();
			if (pAvailableKeys->f_IsEmpty())
				m_Database.m_AvailableKeys.f_Remove(_KeySize);

			_AppAuditor.f_Info("Used pre-created key of {} bytes for ID '{}'. There are now {} pre-created keys left."_f << _KeySize << _Identifier << KeysLeft);
			co_return fg_Move(TryKey);
		}

		co_return {};
	}

	NConcurrency::TCFuture<CSymmetricKey> CKeyManagerServer::CInternal::CKeyManagerImplementation::f_RequestKey(CStr const &_Identifier, uint32 _KeySize)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		auto HostID = NConcurrency::CActorDistributionManager::fs_GetCallingHostInfo().f_GetRealHostID();

		auto &Client = Internal.m_Database.m_Clients[HostID];
		if (auto pKey = Client.m_Keys.f_FindEqual(_Identifier))
		{
			if (pKey->m_Key.f_GetLen() != _KeySize)
				co_return AppAuditor.f_Exception("Saved key has different size {} from requested size {}"_f << pKey->m_Key.f_GetLen() << _KeySize);

			auto nVerifiedServers = pKey->m_VerifiedOnServers.f_GetLen();
			if (nVerifiedServers < Internal.m_Config.m_CreateNewKeyMinServers)
				co_return AppAuditor.f_Exception("Key has only been verified on {} of {} required servers"_f << nVerifiedServers << Internal.m_Config.m_CreateNewKeyMinServers);

			AppAuditor.f_Info("Returned key of {} bytes with ID '{}'"_f << _KeySize << _Identifier);

			co_return pKey->m_Key;
		}
		else
		{
			auto NewKeyFromAvailableKeys = co_await Internal.f_TryUseAvailableKey(_Identifier, _KeySize, AppAuditor);

			auto &KeyStore = Client.m_Keys[_Identifier];

			if (NewKeyFromAvailableKeys)
				KeyStore.m_Key = fg_Move(*NewKeyFromAvailableKeys);
			else
			{
				KeyStore.m_Key.f_SetLen(_KeySize);
				NSys::fg_Security_GenerateHighEntropyData(KeyStore.m_Key.f_GetArray(), KeyStore.m_Key.f_GetLen());
				AppAuditor.f_Info("Created new key of {} bytes for ID '{}'"_f << _KeySize << _Identifier);
			}

			KeyStore.m_VerifiedOnServers[Internal.m_ThisHostID];

			CKeyManagerServerSync::CHostKeyID HostKeyID{.m_HostID = HostID, .m_KeyID = _Identifier};

			NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> ForwardCreateNewKeys;
			ForwardCreateNewKeys[HostKeyID] = KeyStore.m_Key;

			auto ForwardVerified = co_await Internal.f_ForwardCreateNewKeys(Internal.m_ThisHostID, ForwardCreateNewKeys);

			ForwardVerified[HostKeyID][Internal.m_ThisHostID];

			auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
			if (!WriteResult)
				co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

			co_await Internal.f_ForwardVerifiedHosts(ForwardVerified);

			auto pClient = Internal.m_Database.m_Clients.f_FindEqual(HostID);
			if (!pClient)
				co_return AppAuditor.f_Exception("Client no longer available");

			pKey = pClient->m_Keys.f_FindEqual(_Identifier);
			if (!pKey)
				co_return AppAuditor.f_Exception("Key no longer available");

			auto nVerifiedServers = pKey->m_VerifiedOnServers.f_GetLen();
			if (nVerifiedServers < Internal.m_Config.m_CreateNewKeyMinServers)
				co_return AppAuditor.f_Exception("Key has only been verified on {} of {} required servers"_f << nVerifiedServers << Internal.m_Config.m_CreateNewKeyMinServers);

			co_return pKey->m_Key;
		}
	}

	auto CKeyManagerServer::CInternal::CKeyManagerImplementation::f_GetServerSyncInterface(NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
		-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<CKeyManagerServerSync>>
	{
		auto &Internal = *m_pThis->mp_pInternal;

		auto Auditor = Internal.m_Config.m_fAuditorFactory(fg_GetCallingHostInfo(), "KeyManager");

		NContainer::TCVector<CStr> Permissions{"KeyManager/ServerSync"};

		if (!co_await Internal.m_Permissions.f_HasPermission("Get Server Sync Interface", Permissions, Auditor.f_HostInfo()))
			co_return Auditor.f_AccessDenied("(Get Server Sync Interface)", Permissions);
		
		TCDistributedActorInterfaceWithID<CKeyManagerServerSync> SyncInterface
			{
				Internal.m_KeyManagerServerSyncInstance.m_Actor->f_ShareInterface<CKeyManagerServerSync>()
				, g_ActorSubscription / []() mutable -> TCFuture<void>
				{
					co_return {};
				}
			}
		;
		
		co_return fg_Move(SyncInterface);
	}
}
