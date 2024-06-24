// Copyright © 2024 Favro Holding AB
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

	auto CKeyManagerServer::CInternal::f_ForwardCreateNewKeys(CStr _FromHostID, NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> _CreateNewKeys)
		-> NConcurrency::TCFuture<NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>>>
	{
		if (_CreateNewKeys.f_IsEmpty())
			co_return {};

		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> ForwardVerified;
		for (auto &OtherKeyManager : m_OtherKeyManagers)
		{
			if (OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID == _FromHostID)
				continue;

			auto Result = co_await OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_CreateNewKeys)(_CreateNewKeys).f_Wrap();
			if (!Result)
			{
				DMibLogWithCategory
					(
						Mib/Cloud/KeyManagerServer
						, Error
						, "Failed to create new keys in remote key manager '{}': {}"
						, OtherKeyManager.m_ActorInfo.m_HostInfo
						, Result.f_GetExceptionStr()
					)
				;
				continue;
			}

			for (auto &HostKeyID : _CreateNewKeys.f_Keys())
			{
				auto &LocalKey = m_Database.m_Clients[HostKeyID.m_HostID].m_Keys[HostKeyID.m_KeyID];
				if (LocalKey.m_VerifiedOnServers(OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID).f_WasCreated())
					ForwardVerified[HostKeyID][OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID];
			}
		}

		co_return ForwardVerified;
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_ForwardRemovePreCreatedKeys(NStr::CStr _FromHostID, NContainer::TCSet<CSymmetricKey> _Keys)
	{
		if (_Keys.f_IsEmpty())
			co_return {};

		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		for (auto &OtherKeyManager : m_OtherKeyManagers)
		{
			if (OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID == _FromHostID)
				continue;

			if (OtherKeyManager.m_ServerSync->f_InterfaceVersion() < EKeyManagerProtocolVersion_SupportRemovePreCreatedKeys)
				continue;

			auto Result = co_await OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_RemovePreCreatedKeys)(_Keys).f_Wrap();
			if (!Result)
			{
				DMibLogWithCategory
					(
						Mib/Cloud/KeyManagerServer
						, Error
						, "Failed to remove pre-created keys in remote key manager '{}': {}"
						, OtherKeyManager.m_ActorInfo.m_HostInfo
						, Result.f_GetExceptionStr()
					)
				;
				continue;
			}
		}

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_ForwardPreCreateKeys(NStr::CStr _FromHostID, NContainer::TCSet<CSymmetricKey> _PreCreateKeys)
	{
		if (_PreCreateKeys.f_IsEmpty())
			co_return {};

		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		for (auto &OtherKeyManager : m_OtherKeyManagers)
		{
			if (OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID == _FromHostID)
				continue;

			auto Result = co_await OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_PreCreateKeys)(_PreCreateKeys).f_Wrap();
			if (!Result)
			{
				DMibLogWithCategory
					(
						Mib/Cloud/KeyManagerServer
						, Error
						, "Failed to pre-create new keys in remote key manager '{}': {}"
						, OtherKeyManager.m_ActorInfo.m_HostInfo
						, Result.f_GetExceptionStr()
					)
				;
				continue;
			}
		}

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_ForwardVerifiedHosts
		(
			NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> _KeysVerifiedOnServers
		)
	{
		if (_KeysVerifiedOnServers.f_IsEmpty())
			co_return {};

		TCActorResultVector<void> ReportResults;

		for (auto &OtherKeyManager : m_OtherKeyManagers)
			OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_KeysVerifiedOnServers)(_KeysVerifiedOnServers) > ReportResults.f_AddResult();

		co_await (ReportResults.f_GetUnwrappedResults() % "Failed to report keys verified on remote key managers");

		co_return {};
	}

	auto CKeyManagerServer::CInternal::f_ForwardRemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs, NContainer::TCSet<NStr::CStr> _CheckedServers)
		-> NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>>
	{
		if (_HostIDs.f_IsEmpty())
			co_return {};
		
		TCActorResultVector<NContainer::TCSet<NStr::CStr>> ReportResults;

		for (auto &OtherKeyManager : m_OtherKeyManagers)
		{
			if (_CheckedServers.f_FindEqual(OtherKeyManager.m_ActorInfo.m_HostInfo.m_HostID))
				continue;

			OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_RemoveVerifiedHosts)(_HostIDs, _CheckedServers) > ReportResults.f_AddResult();
		}

		auto Results = co_await ReportResults.f_GetUnwrappedResults();

		NContainer::TCSet<NStr::CStr> RemovedIDs;

		for (auto &Result : Results)
			RemovedIDs += Result;

		co_return fg_Move(RemovedIDs);
	}

	NConcurrency::TCFuture<NStorage::TCOptional<NConcurrency::CActorSubscription>> CKeyManagerServer::CInternal::f_UseAvailableKey(CSymmetricKey _Key)
	{
		TCActorResultMap<CHostInfo, CKeyManagerServerSync::CUseAvailableKeyResult> Results;

		mint nTotalKeyManagers = m_OtherKeyManagers.f_GetLen() + 1;
		if (nTotalKeyManagers < m_Config.m_CreateNewKeyMinServers)
			co_return DMibErrorInstance("Only {} of {} key managers available to verify available key use"_f << nTotalKeyManagers << m_Config.m_CreateNewKeyMinServers);

		for (auto &OtherKeyManager : m_OtherKeyManagers)
			OtherKeyManager.m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_UseAvailableKey)(_Key) > Results.f_AddResult(OtherKeyManager.m_ActorInfo.m_HostInfo);

		auto UnwrappedResults = co_await (Results.f_GetResults() % "Failed to use available key on remote key managers");

		mint nVerified = 1;
		for (auto &Result : UnwrappedResults.f_Entries())
		{
			if (!Result.f_Value())
			{
				DMibLogWithCategory
					(
						Mib/Cloud/KeyManagerServer
						, Error
						, "Failed to use available key in remote key manager '{}': {}"
						, Result.f_Key()
						, Result.f_Value().f_GetExceptionStr()
					)
				;
				continue;
			}

			if (!Result.f_Value()->m_bCanUse)
				co_return {};

			++nVerified;
		}

		if (nVerified < m_Config.m_CreateNewKeyMinServers)
			co_return DMibErrorInstance("Could only verify available key use on {} of {} required key managers"_f << nVerified << m_Config.m_CreateNewKeyMinServers);

		co_return g_ActorSubscription / [ResultsReference = fg_Move(UnwrappedResults)]
			{
			}
		;
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_SyncFromOtherKeyManager(NConcurrency::TCWeakDistributedActor<CActor> _KeyManager)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		COtherKeyManager *pOtherKeyManager = nullptr;

		auto CheckOtherKeyManager = co_await fg_OnResume
			(
				[&]() -> NException::CExceptionPointer
				{
					pOtherKeyManager = m_OtherKeyManagers.f_FindEqual(_KeyManager);
					if (!pOtherKeyManager)
						return DMibErrorInstance("Remote key manager no longer available");

					return {};
				}
			)
		;

		bool bDatabaseChanged = false;
		auto ReadDatabase = co_await pOtherKeyManager->m_ServerSync.f_CallActor(&CKeyManagerServerSync::f_ReadDatabase)();

		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> ReportVerified;
		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> CreateNewKeys;

		for (auto iClient = co_await fg_Move(ReadDatabase.m_Clients).f_GetIterator(); iClient; co_await ++iClient)
		{
			auto &&Client = *iClient;

			auto MappedClient = m_Database.m_Clients(Client.m_HostID);
			if (MappedClient.f_WasCreated())
				bDatabaseChanged = true;
			auto &LocalClient = *MappedClient;

			for (auto iKey = co_await fg_Move(Client.m_Keys).f_GetIterator(); iKey; co_await ++iKey)
			{
				auto &&Key = *iKey;
				auto MappedKey = LocalClient.m_Keys(Key.m_KeyID);
				auto &LocalKey = *MappedKey;

				CKeyManagerServerSync::CHostKeyID HostKeyID{.m_HostID = Client.m_HostID, .m_KeyID = Key.m_KeyID};

				if (MappedKey.f_WasCreated())
				{
					bDatabaseChanged = true;
					LocalKey.m_Key = fg_Move(Key.m_Key);
					LocalKey.m_VerifiedOnServers = Key.m_VerifiedOnServers;
					LocalKey.m_VerifiedOnServers[m_ThisHostID];
					ReportVerified[HostKeyID] = LocalKey.m_VerifiedOnServers;
					CreateNewKeys[HostKeyID] = LocalKey.m_Key;

					DMibLogWithCategory
						(
							Mib/Cloud/KeyManagerServer
							, Info
							, "Synced key for host '{}' with ID '{}' from '{}'"
							, Client.m_HostID
							, Key.m_KeyID
							, pOtherKeyManager->m_ActorInfo.m_HostInfo
						)
					;
				}
				else if (LocalKey.m_Key != Key.m_Key)
				{
					DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Critical, "Key contents mismatch between this host and '{}'", pOtherKeyManager->m_ActorInfo.m_HostInfo);
					continue;
				}

				for (auto &VerifiedOnHostID : Key.m_VerifiedOnServers)
				{
					if (LocalKey.m_VerifiedOnServers(VerifiedOnHostID).f_WasCreated())
					{
						ReportVerified[HostKeyID][VerifiedOnHostID];
						bDatabaseChanged = true;
					}
				}

				if (LocalKey.m_VerifiedOnServers(m_ThisHostID).f_WasCreated())
				{
					ReportVerified[HostKeyID][m_ThisHostID];
					bDatabaseChanged = true;
				}
			}
		}

		NContainer::TCMap<CSymmetricKey, CKeyManagerServerSync::CHostKeyID> UsedKeys;

		for (auto &Client : m_Database.m_Clients)
		{
			for (auto &Key: Client.m_Keys)
				UsedKeys[Key.m_Key] = {.m_HostID = Client.f_GetID(), .m_KeyID = Key.f_GetID()};
		}

		NContainer::TCSet<CSymmetricKey> ForwardPreCreateKeys;
		{
			NContainer::TCSet<uint32> Sizes;
			NContainer::TCMap<uint32, NContainer::TCSet<CSymmetricKey>> PotentiallyAvailableKeys;
			for (auto iAvailableKeys = co_await fg_Move(ReadDatabase.m_AvailableKeys).f_GetIterator(); iAvailableKeys; co_await ++iAvailableKeys)
			{
				auto &&AvailableKey = *iAvailableKeys;
				auto KeySize = AvailableKey.m_KeySize;
				Sizes[KeySize];
				for (auto iKey = co_await fg_Move(AvailableKey.m_Keys).f_GetIterator(); iKey; co_await ++iKey)
					PotentiallyAvailableKeys[KeySize][*iKey];
			}

			for (auto &Size : m_Database.m_AvailableKeys.f_Keys())
				Sizes[Size];

			for (auto &Size : Sizes)
			{
				NContainer::TCSet<CSymmetricKey> AlreadyExistingKeys;
				NContainer::TCSet<CSymmetricKey> NewKeys;

				auto fAddKeys = [&](auto &_Keys)
					{
						mint nAdded = 0;
						for (auto &Key : _Keys)
						{
							if (!AlreadyExistingKeys(Key).f_WasCreated())
								continue;

							if (auto *pUsed = UsedKeys.f_FindEqual(Key))
							{
								CreateNewKeys[*pUsed] = Key; // Force remote to remove from available keys
								continue;
							}

							++nAdded;
							NewKeys[Key];
						}

						return nAdded;
					}
				;

				auto *pRemoteKeys = PotentiallyAvailableKeys.f_FindEqual(Size);

				if (auto *pLocalKeys = m_Database.m_AvailableKeys.f_FindEqual(Size))
				{
					fAddKeys(*pLocalKeys);

					if (pRemoteKeys)
					{
						for (auto &LocalKey : *pLocalKeys)
						{
							if (!pRemoteKeys->f_FindEqual(LocalKey))
								ForwardPreCreateKeys[LocalKey];
						}
					}
				}

				if (pRemoteKeys)
				{
					auto nAdded = fAddKeys(*pRemoteKeys);

					if (nAdded)
						DMibLogWithCategory(Mib/Cloud/KeyManagerServer, Info, "Synced {} new available keys of size {} from '{}'", nAdded, Size, pOtherKeyManager->m_ActorInfo.m_HostInfo);
				}

				auto &Keys = m_Database.m_AvailableKeys[Size];
				if (Keys != NewKeys)
				{
					Keys = fg_Move(NewKeys);
					bDatabaseChanged = true;
				}

				if (Keys.f_IsEmpty())
					m_Database.m_AvailableKeys.f_Remove(Size);
			}
		}

		if (!ForwardPreCreateKeys.f_IsEmpty())
			co_await f_ForwardPreCreateKeys({}, ForwardPreCreateKeys);

		auto NewVerified = co_await f_ForwardCreateNewKeys({}, CreateNewKeys);
		if (!NewVerified.f_IsEmpty())
			bDatabaseChanged = true;

		for (auto &Verified : NewVerified.f_Entries())
			ReportVerified[Verified.f_Key()] += Verified.f_Value();

		if (bDatabaseChanged)
			co_await m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, m_Database);

		if (!ReportVerified.f_IsEmpty())
			co_await f_ForwardVerifiedHosts(ReportVerified);

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::f_SetupServerSync()
	{
		m_OtherKeyManagersSubscription = co_await m_Config.m_TrustManager->f_SubscribeTrustedActors<CKeyManager>();

		co_await m_OtherKeyManagersSubscription.f_OnActor
			(
				g_ActorFunctor / [this](TCDistributedActor<CKeyManager> const &_KeyManager, CTrustedActorInfo const &_ActorInfo) -> TCFuture<void>
				{
					if (_KeyManager->f_InterfaceVersion() < EKeyManagerProtocolVersion_SupportServerSync)
						co_return {};

					auto ServerSyncInterface = co_await _KeyManager.f_CallActor(&CKeyManager::f_GetServerSyncInterface)(g_ActorSubscription / [] {});

					if (ServerSyncInterface.f_IsEmpty())
						co_return DMibErrorInstance("Remote KeyManager returned an empty server sync interface");

					auto &OtherKeyManager = m_OtherKeyManagers[_KeyManager];
					OtherKeyManager.m_ServerSync = fg_Move(ServerSyncInterface);
					OtherKeyManager.m_ActorInfo = _ActorInfo;

					co_await f_SyncFromOtherKeyManager(_KeyManager.f_Weak());

					co_return {};
				}
				, g_ActorFunctor / [this](TCWeakDistributedActor<CActor> const &_KeyManager, CTrustedActorInfo &&_ActorInfo) -> TCFuture<void>
				{
					if (auto pManager = m_OtherKeyManagers.f_FindEqual(_KeyManager))
					{
						auto ServerSync = fg_Move(pManager->m_ServerSync);
						m_OtherKeyManagers.f_Remove(pManager);

						co_await fg_Move(ServerSync).f_Destroy();
					}

					co_return {};
				}
				, "KeyManager"
				, "Failed to handle '{}' for other key managers"
			)
		;

		co_return {};
	}

	NConcurrency::TCFuture<CKeyManagerServerSync::CReadDatabase> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_ReadDatabase()
	{
		auto &Internal = *m_pThis->mp_pInternal;

		NStorage::TCSharedPointer<ICKeyManagerServerDatabase::CDatabase const> pDatabase = fg_Construct(Internal.m_Database);
		co_return CReadDatabase
			{
				.m_Clients = fg_CallSafe
				(
					[this, pDatabase]() -> TCAsyncGenerator<CReadDatabase::CClient>
					{
						auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

						for (auto &Client : pDatabase->m_Clients)
						{
							co_yield CReadDatabase::CClient
								{
									.m_HostID = Client.f_GetID()
									, .m_Keys = fg_CallSafe
									(
										[this, pDatabase, pClient = &Client]() -> TCAsyncGenerator<CReadDatabase::CClientKey>
										{
											auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

											for (auto &KeyEntry : pClient->m_Keys.f_Entries())
											{
												co_yield CReadDatabase::CClientKey
													{
														.m_KeyID = KeyEntry.f_Key()
														, .m_Key = KeyEntry.f_Value().m_Key
														, .m_VerifiedOnServers = KeyEntry.f_Value().m_VerifiedOnServers
													}
												;
											}

											co_return {};
										}
									)
								}
							;
						}

						co_return {};
					}
				)
				, .m_AvailableKeys = fg_CallSafe
				(
					[this, pDatabase]() -> TCAsyncGenerator<CReadDatabase::CAvailableKeys>
					{
						auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

						for (auto &AvailableKeyEntry : pDatabase->m_AvailableKeys.f_Entries())
						{
							co_yield CReadDatabase::CAvailableKeys
								{
									.m_KeySize = AvailableKeyEntry.f_Key()
									, .m_Keys = fg_CallSafe
									(
										[this, pDatabase, pAvailableKeyEntry = &AvailableKeyEntry]() -> TCAsyncGenerator<CSymmetricKey>
										{
											auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

											for (auto &Key : pAvailableKeyEntry->f_Value())
												co_yield Key;

											co_return {};
										}
									)
								}
							;
						}

						co_return {};
					}
				)
			}
		;
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_PreCreateKeys(NContainer::TCSet<CSymmetricKey> &&_Keys)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		NContainer::TCSet<CSymmetricKey> ForwardPreCreateKeys;

		for (auto &Key : _Keys)
		{
			mint KeySize = Key.f_GetLen();
			auto &AvailableKeys = Internal.m_Database.m_AvailableKeys[KeySize];
			if (AvailableKeys(Key).f_WasCreated())
				ForwardPreCreateKeys[Key];
		}

		if (ForwardPreCreateKeys.f_IsEmpty())
			co_return {};

		co_await Internal.f_ForwardPreCreateKeys(CallingHostInfo.f_GetRealHostID(), ForwardPreCreateKeys);

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_RemovePreCreatedKeys(NContainer::TCSet<CSymmetricKey> &&_Keys)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		NContainer::TCSet<CSymmetricKey> ForwardRemoveKeys;

		for (auto &Key : _Keys)
		{
			mint KeySize = Key.f_GetLen();
			auto *pKeys = Internal.m_Database.m_AvailableKeys.f_FindEqual(KeySize);
			if (!pKeys)
				continue;

			if (pKeys->f_Remove(Key))
				ForwardRemoveKeys[Key];
		}

		if (ForwardRemoveKeys.f_IsEmpty())
			co_return {};

		co_await Internal.f_ForwardRemovePreCreatedKeys(CallingHostInfo.f_GetRealHostID(), ForwardRemoveKeys);

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		co_return {};
	}

	NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_RemoveVerifiedHosts
		(
			NContainer::TCSet<NStr::CStr> &&_HostIDs
			, NContainer::TCSet<NStr::CStr> &&_CheckedServers
		)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		if (_HostIDs.f_FindEqual(Internal.m_ThisHostID))
			co_return AppAuditor.f_Exception("The host ID {} you are trying to remove is still running"_f << Internal.m_ThisHostID);

		if (!_CheckedServers(Internal.m_ThisHostID).f_WasCreated())
			co_return {};

		auto RemovedHosts = co_await Internal.f_ForwardRemoveVerifiedHosts(_HostIDs, _CheckedServers);

		for (auto &Client : Internal.m_Database.m_Clients)
		{
			for (auto &Key : Client.m_Keys)
			{
				for (auto &RemoveKey : _HostIDs)
				{
					if (Key.m_VerifiedOnServers.f_Remove(RemoveKey))
						RemovedHosts[RemoveKey];
				}
			}
		}

		if (RemovedHosts.f_IsEmpty())
			co_return {};

		AppAuditor.f_Info("Removed {} verified host IDs"_f << RemovedHosts.f_GetLen());

		auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
		if (!WriteResult)
			co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});

		co_return fg_Move(RemovedHosts);
	}

	auto CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_UseAvailableKey(CSymmetricKey &&_Key) -> NConcurrency::TCFuture<CUseAvailableKeyResult>
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto HostID = CallingHostInfo.f_GetRealHostID();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		if (!Internal.m_Database.f_HasAvailableKey(_Key))
			co_return CUseAvailableKeyResult{.m_bCanUse = false};

		auto Mapping = Internal.m_TryingToUseAvailableKeys(_Key, HostID);
		auto &MappedHostID = *Mapping;
		if (!Mapping.f_WasCreated())
		{
			if (HostID < MappedHostID)
				MappedHostID = HostID;
			else
				co_return CUseAvailableKeyResult{.m_bCanUse = false};
		}

		co_return CUseAvailableKeyResult
			{
				.m_bCanUse = true
				, .m_Subscription = g_ActorSubscription / [this, _Key, HostID]() -> TCFuture<void>
				{
					auto &Internal = *m_pThis->mp_pInternal;
					auto *pHostID = Internal.m_TryingToUseAvailableKeys.f_FindEqual(_Key);
					if (pHostID && *pHostID == HostID)
						Internal.m_TryingToUseAvailableKeys.f_Remove(pHostID);

					co_return {};
				}
			}
		;
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_CreateNewKeys(NContainer::TCMap<CHostKeyID, CSymmetricKey> &&_Keys)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		bool bDatabaseChanged = false;

		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> ForwardCreateNewKeys;
		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> ForwardVerified;

		for (auto &KeyEntry : _Keys.f_Entries())
		{
			auto &KeyID = KeyEntry.f_Key();
			auto &Key = KeyEntry.f_Value();

			auto &Host = Internal.m_Database.m_Clients[KeyID.m_HostID];
			auto KeyMapping = Host.m_Keys(KeyID.m_KeyID, ICKeyManagerServerDatabase::CDatabase::CClientKey{.m_Key = Key});
			if (!KeyMapping.f_WasCreated())
			{
				if ((*KeyMapping).m_Key != Key)
					co_return DMibErrorInstance(AppAuditor.f_Critical("Conflict when creating new key for host '{}' with ID '{}'"_f << KeyID.m_HostID << KeyID.m_KeyID));

				continue;
			}
			else
			{
				bDatabaseChanged = true;
				ForwardCreateNewKeys(KeyID, Key);
				(*KeyMapping).m_VerifiedOnServers[Internal.m_ThisHostID];
				ForwardVerified[KeyID][Internal.m_ThisHostID];

				AppAuditor.f_Info
					(
						"Created new key for host '{}' with ID '{}'. Received via server sync"_f
						<< KeyID.m_HostID
						<< KeyID.m_KeyID
					)
				;
			}

			auto pAvailableKeys = Internal.m_Database.m_AvailableKeys.f_FindEqual(Key.f_GetLen());
			if (pAvailableKeys)
			{
				auto *pExistingKey = pAvailableKeys->f_FindEqual(Key);
				if (pExistingKey)
				{
					pAvailableKeys->f_Remove(pExistingKey);
					bDatabaseChanged = true;

					mint nAvailable = pAvailableKeys->f_GetLen();
					AppAuditor.f_Info
						(
							"Removed pre-created key of {} bytes as result of another server using it for host '{}' with ID '{}'. There are now {} keys left."_f
							<< Key.f_GetLen()
							<< KeyID.m_HostID
							<< KeyID.m_KeyID
							<< nAvailable
						)
					;

					if (nAvailable == 0)
						Internal.m_Database.m_AvailableKeys.f_Remove(pAvailableKeys);
				}
			}
		}

		ForwardVerified += co_await Internal.f_ForwardCreateNewKeys(CallingHostInfo.f_GetRealHostID(), ForwardCreateNewKeys);

		if (!ForwardVerified.f_IsEmpty())
			bDatabaseChanged = true;

		if (bDatabaseChanged)
		{
			auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
			if (!WriteResult)
				co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});
		}

		if (!ForwardVerified.f_IsEmpty())
			co_await Internal.f_ForwardVerifiedHosts(ForwardVerified);

		co_return {};
	}

	NConcurrency::TCFuture<void> CKeyManagerServer::CInternal::CKeyManagerServerSyncImplementation::f_KeysVerifiedOnServers
		(
			NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> &&_KeysVerifiedOnServers
		)
	{
		auto CheckDestroy = co_await m_pThis->f_CheckDestroyedOnResume();

		auto &Internal = *m_pThis->mp_pInternal;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto AppAuditor = Internal.m_Config.m_fAuditorFactory(CallingHostInfo, "KeyManager");

		bool bDatabaseChanged = false;

		NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> ForwardVerified;

		for (auto &HostKeyEntry : _KeysVerifiedOnServers.f_Entries())
		{
			auto &HostKeyId = HostKeyEntry.f_Key();
			auto &VerifiedHosts = HostKeyEntry.f_Value();

			auto &LocalVerifiedHosts = Internal.m_Database.m_Clients[HostKeyId.m_HostID].m_Keys[HostKeyId.m_KeyID].m_VerifiedOnServers;

			for (auto &HostID : VerifiedHosts)
			{
				if (LocalVerifiedHosts(HostID).f_WasCreated())
				{
					ForwardVerified[HostKeyId][HostID];
					bDatabaseChanged = true;
				}
			}
		}

		if (bDatabaseChanged)
		{
			auto WriteResult = co_await Internal.m_Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_WriteDatabase, Internal.m_Database).f_Wrap();
			if (!WriteResult)
				co_return AppAuditor.f_CriticalException({"Failed to write database", WriteResult.f_GetExceptionStr()});
		}

		if (!ForwardVerified.f_IsEmpty())
			co_await Internal.f_ForwardVerifiedHosts(ForwardVerified);
		
		co_return {};
	}

	TCFuture<void> CKeyManagerServer::CInternal::f_SetupPermissions()
	{
		NContainer::TCSet<CStr> Permissions{"KeyManager/ServerSync"};
		co_await m_Config.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));

		NContainer::TCVector<CStr> SubscribePermissions{"KeyManager/ServerSync"};
		m_Permissions = co_await m_Config.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, fg_Move(SubscribePermissions), fg_ThisActor(m_pThis));

		co_return {};
	}
}
