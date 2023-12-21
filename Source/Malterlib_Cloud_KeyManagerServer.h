// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedApp>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib::NCloud
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
			NContainer::TCMap<uint32, NContainer::TCVector<CSymmetricKey>> m_AvailableKeys;
			
			template <typename tf_CStream>
			void f_Feed(tf_CStream &_Stream) const
			{
				_Stream << uint32(EVersion);
				_Stream << m_Clients;
				_Stream << m_AvailableKeys;
			}
				
			template <typename tf_CStream>
			void f_Consume(tf_CStream &_Stream)
			{
				uint32 Version = 0;
				_Stream >> Version;
				_Stream >> m_Clients;
				_Stream >> m_AvailableKeys;
			}
		};
		
		virtual NConcurrency::TCFuture<void> f_Initialize() = 0;
		virtual NConcurrency::TCFuture<void> f_WriteDatabase(CDatabase const &_Database) = 0;
		virtual NConcurrency::TCFuture<CDatabase> f_ReadDatabase() = 0;
	};
	
	struct CKeyManagerServerConfig
	{
		NConcurrency::TCActor<ICKeyManagerServerDatabase> m_DatabaseActor;
		NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> m_TrustManager;
		NFunction::TCFunctionMovable<NConcurrency::CDistributedAppAuditor (NConcurrency::CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> m_fAuditorFactory
			= [](NConcurrency::CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category) -> NConcurrency::CDistributedAppAuditor
			{
				return {};
			}
		;
	};

	struct CKeyManagerServer : public NConcurrency::CActor
	{
		CKeyManagerServer(CKeyManagerServerConfig &&_Config);
		~CKeyManagerServer();
		
		NConcurrency::TCFuture<void> f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys);
		NConcurrency::TCFuture<void> f_Init();

	private:

		NConcurrency::TCFuture<void> fp_Destroy() override;
		
		struct CInternal;
		NStorage::TCUniquePointer<CInternal> mp_pInternal;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
