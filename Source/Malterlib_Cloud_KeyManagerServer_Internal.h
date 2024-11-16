// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/WeakActor>

namespace NMib::NCloud
{
	struct CKeyManagerServer::CInternal : public NConcurrency::CActorInternal
	{
		struct CKeyManagerImplementation : public CKeyManager
		{
			NConcurrency::TCFuture<CSymmetricKey> f_RequestKey(NStr::CStr _Identifier, uint32 _KeySize) override;
			auto f_GetServerSyncInterface(NConcurrency::TCActorSubscriptionWithID<> _Subscription)
				-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<CKeyManagerServerSync>>
			override;

			DMibDelegatedActorImplementation(CKeyManagerServer);
		};

		struct CKeyManagerServerSyncImplementation : public CKeyManagerServerSync
		{
			NConcurrency::TCFuture<CReadDatabase> f_ReadDatabase() override;
			NConcurrency::TCFuture<void> f_CreateNewKeys(NContainer::TCMap<CHostKeyID, CSymmetricKey> _Keys) override;
			NConcurrency::TCFuture<CUseAvailableKeyResult> f_UseAvailableKey(CSymmetricKey _Key) override;
			NConcurrency::TCFuture<void> f_PreCreateKeys(NContainer::TCSet<CSymmetricKey> _Keys) override;
			NConcurrency::TCFuture<void> f_RemovePreCreatedKeys(NContainer::TCSet<CSymmetricKey> _Keys) override;
			NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_RemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs, NContainer::TCSet<NStr::CStr> _CheckedServers) override;
			NConcurrency::TCFuture<void> f_KeysVerifiedOnServers(NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> _KeysVerifiedOnServers) override;

			DMibDelegatedActorImplementation(CKeyManagerServer);
		};

		struct COtherKeyManager
		{
			NConcurrency::CTrustedActorInfo m_ActorInfo;
			NConcurrency::TCDistributedActorInterface<CKeyManagerServerSync> m_ServerSync;
		};

		CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig &&_Config);

		NConcurrency::TCFuture<void> f_ReadDatabase();
		NConcurrency::TCFuture<void> f_SetupPermissions();
		NConcurrency::TCFuture<void> f_SetupServerSync();
		NConcurrency::TCFuture<void> f_SyncFromOtherKeyManager(NConcurrency::TCWeakDistributedActor<CActor> _KeyManager);
		auto f_ForwardCreateNewKeys(NStr::CStr _FromHostID, NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, CSymmetricKey> _CreateNewKeys)
			-> NConcurrency::TCFuture<NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>>>
		;
		NConcurrency::TCFuture<void> f_ForwardPreCreateKeys(NStr::CStr _FromHostID, NContainer::TCSet<CSymmetricKey> _PreCreateKeys);
		NConcurrency::TCFuture<void> f_ForwardRemovePreCreatedKeys(NStr::CStr _FromHostID, NContainer::TCSet<CSymmetricKey> _Keys);
		NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_ForwardRemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs, NContainer::TCSet<NStr::CStr> _CheckedServers);
		NConcurrency::TCFuture<void> f_ForwardVerifiedHosts(NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> _KeysVerifiedOnServers);
		NConcurrency::TCFuture<NStorage::TCOptional<NConcurrency::CActorSubscription>> f_UseAvailableKey(CSymmetricKey _Key);
		NConcurrency::TCFuture<NStorage::TCOptional<CSymmetricKey>> f_TryUseAvailableKey(NStr::CStr _Identifier, uint32 _KeySize, NConcurrency::CDistributedAppAuditor _AppAuditor);

		CKeyManagerServer *m_pThis;
		NStr::CStr m_ThisHostID;
		CKeyManagerServerConfig m_Config;
		NConcurrency::TCActor<NConcurrency::CActorDistributionManager> m_DistributionManager;
		ICKeyManagerServerDatabase::CDatabase m_Database;
		NConcurrency::TCDistributedActorInstance<CKeyManagerImplementation> m_KeyManagerInstance;
		NConcurrency::TCDistributedActorInstance<CKeyManagerServerSyncImplementation> m_KeyManagerServerSyncInstance;
		NConcurrency::TCTrustedActorSubscription<CKeyManager> m_OtherKeyManagersSubscription;
		NContainer::TCMap<NConcurrency::TCWeakDistributedActor<CActor>, COtherKeyManager> m_OtherKeyManagers;
		NContainer::TCMap<CSymmetricKey, NStr::CStr> m_TryingToUseAvailableKeys;
		NConcurrency::CTrustedPermissionSubscription m_Permissions;
	};
}
