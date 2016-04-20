#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib
{
	namespace NCloud
	{
		class ICKeyManagerServerDatabase : public NConcurrency::CActor
		{
		public:
			struct CDatabase
			{
				struct CClientStore
				{
					NContainer::TCMap<NStr::CStrSecure, CSymmetricKey> m_Keys;
					
					NStr::CStrSecure const &f_GetID() const
					{
						return NContainer::TCMap<NStr::CStrSecure, CClientStore>::fs_GetKey(*this);
					}
				};
				
				NContainer::TCMap<NStr::CStrSecure, CClientStore> m_Clients;
			};
			
			virtual NConcurrency::TCContinuation<void> f_WriteDatabase(CDatabase const &_Database) pure;
			virtual NConcurrency::TCContinuation<CDatabase> f_ReadDatabase() pure;
		};
		
		struct CKeyManagerServerConfig
		{
			NConcurrency::TCActor<ICKeyManagerServerDatabase> m_DatabaseActor;
			NContainer::TCSet<NContainer::TCVector<uint8>> m_PublicKeysForAllKeyManagers;
		};

		class CKeyManagerServer : public NConcurrency::CActor
		{
		public:
			CKeyManagerServer();
			~CKeyManagerServer();
			CKeyManagerServer(CKeyManagerServerConfig const &_Config);
			
		private:
			struct CInternal;
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}
