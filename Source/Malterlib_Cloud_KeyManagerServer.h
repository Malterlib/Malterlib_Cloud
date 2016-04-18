#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib
{
	namespace NCloud
	{
		class CKeyManagerServerDatabase : public NConcurrency::CActor
		{
			struct CDatabase
			{
				NContainer::TCMap<NStr::CStrSecure, CSymmetricKey> m_Keys; 
			};
			
			virtual NConcurrency::TCContinuation<void> f_WriteDatabase(CDatabase const &_Database) pure;
			virtual NConcurrency::TCContinuation<CDatabase> f_ReadDatabase() pure;
		};
		
		struct CKeyManagerServerConfig
		{
			NConcurrency::TCActor<CKeyManagerServerDatabase> m_DatabaseActor;
			NContainer::TCSet<NContainer::TCVector<uint8>> m_PublicKeysForAllKeyManagers;
		};

		class CKeyManagerServer : public NConcurrency::CActor
		{
			CKeyManagerServer(CKeyManagerServerConfig const &_Config);
			
		private:
			struct CInternal;
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}
