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
			enum class EVersion : uint32
			{
				mc_Current = 0x100
			};
			
			struct CClientStore
			{
				NStr::CStr const &f_GetID() const;

				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NContainer::TCMap<NStr::CStr, CSymmetricKey> m_Keys;
			};
			
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NContainer::TCMap<NStr::CStr, CClientStore> m_Clients;
			NContainer::TCMap<uint32, NContainer::TCVector<CSymmetricKey>> m_AvailableKeys;
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

#include "Malterlib_Cloud_KeyManagerServer.hpp"
