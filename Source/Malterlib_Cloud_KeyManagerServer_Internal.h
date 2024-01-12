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
			NConcurrency::TCFuture<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize) override;

			DMibDelegatedActorImplementation(CKeyManagerServer);
		};
		
		CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig &&_Config);

		NConcurrency::TCFuture<void> f_ReadDatabase();
		
		CKeyManagerServer *m_pThis;
		CKeyManagerServerConfig m_Config;
		NConcurrency::TCActor<NConcurrency::CActorDistributionManager> m_DistributionManager;
		NStorage::TCUniquePointer<ICKeyManagerServerDatabase::CDatabase> m_pDatabase;
		NContainer::TCVector<NConcurrency::TCPromise<void>> m_OnDatabaseReadyQueue;
		NConcurrency::TCDistributedActorInstance<CKeyManagerImplementation> m_KeyManagerInstance;

		bool m_bReadingDatabase = false;
	};
}
