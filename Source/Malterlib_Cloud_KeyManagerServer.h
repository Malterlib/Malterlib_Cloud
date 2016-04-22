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
					NContainer::TCMap<NStr::CStr, CSymmetricKey> m_Keys;
					
					NStr::CStr const &f_GetID() const
					{
						return NContainer::TCMap<NStr::CStr, CClientStore>::fs_GetKey(*this);
					}
				};
				
				NContainer::TCMap<NStr::CStr, CClientStore> m_Clients;
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
			friend class CKeyManager;
			
		public:
			CKeyManagerServer(CKeyManagerServerConfig const &_Config);
			~CKeyManagerServer();
			
			void f_Construct() override;
			
		protected:
			NConcurrency::TCContinuation<CSymmetricKey> fp_RequestKey(NStr::CStr const &_HostID, NStr::CStr const &_Identifier, uint32 _KeySize);
			
		private:
			struct CInternal;
			NPtr::TCUniquePointer<CInternal> mp_pInternal;
		};
	}
}
