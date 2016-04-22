#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>

namespace NMib
{
	namespace NCloud
	{
		class CKeyManagerInternalInterface : public NConcurrency::CActor
		{
			// Synchronize databases
		};
		
		struct CKeyManagerServer::CInternal
		{
			CInternal(CKeyManagerServer *_pThis, CKeyManagerServerConfig const &_Config);
			
			void f_ReadDatabase(NFunction::TCFunction<void ()> &&_fOnReady, NFunction::TCFunction<void (NStr::CStr const&)> &&_fOnError);
			NConcurrency::TCContinuation<CSymmetricKey> f_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize);
			
			CKeyManagerServer *m_pThis;
			CKeyManagerServerConfig m_Config;
			NConcurrency::TCDistributedActor<CKeyManager> m_KeyManagerActor;
			NConcurrency::CDistributedActorPublication m_KeyManagerPublication;
			NPtr::TCUniquePointer<ICKeyManagerServerDatabase::CDatabase> m_pDatabase;
			NContainer::TCLinkedList<NFunction::TCFunction<void ()>> m_OnDatabaseReadyQueue;
			NContainer::TCLinkedList<NFunction::TCFunction<void (NStr::CStr const&)>> m_OnDatabaseErrorQueue;
		};
		
		struct CKeyManager::CInternal
		{
			CInternal(CKeyManager *_pThis, NConcurrency::TCActor<CKeyManagerServer> const &_ServerActor);
			
			CKeyManager *m_pThis;
			NConcurrency::TCActor<CKeyManagerServer> m_ServerActor;
		};
	}
}
