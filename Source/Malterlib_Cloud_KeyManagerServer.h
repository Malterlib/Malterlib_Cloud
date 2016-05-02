// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

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
				enum
				{
					EVersion = 0x100
				};
				
				struct CClientStore
				{
					NContainer::TCMap<NStr::CStr, CSymmetricKey> m_Keys;
					
					NStr::CStr const &f_GetID() const
					{
						return NContainer::TCMap<NStr::CStr, CClientStore>::fs_GetKey(*this);
					}
					
					template <typename tf_CStream>
					void f_Feed(tf_CStream &_Stream) const
					{
						_Stream << m_Keys;
					}
						
					template <typename tf_CStream>
					void f_Consume(tf_CStream &_Stream)
					{
						_Stream >> m_Keys;
					}
				};
				
				NContainer::TCMap<NStr::CStr, CClientStore> m_Clients;
				
				template <typename tf_CStream>
				void f_Feed(tf_CStream &_Stream) const
				{
					_Stream << uint32(EVersion);
					_Stream << m_Clients;
				}
					
				template <typename tf_CStream>
				void f_Consume(tf_CStream &_Stream)
				{
					uint32 Version = 0;
					_Stream >> Version;
					_Stream >> m_Clients;
				}
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
