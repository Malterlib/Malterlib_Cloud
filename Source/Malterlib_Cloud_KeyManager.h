// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/AsyncGenerator>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib::NCloud
{
	enum EKeyManagerProtocolVersion : uint32
	{
		EKeyManagerProtocolVersion_Min                           = 0x101
		, EKeyManagerProtocolVersion_SupportServerSync           = 0x102
		, EKeyManagerProtocolVersion_SupportRemovePreCreatedKeys = 0x103

		, EKeyManagerProtocolVersion_Current = 0x103
	};

	struct CKeyManagerServerSync : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/KeyManager/ServerSync";

		enum EProtocolVersion : uint32
		{
			EProtocolVersion_Min       = EKeyManagerProtocolVersion_Min
			, EProtocolVersion_Current = EKeyManagerProtocolVersion_Current
		};

		struct CReadDatabase
		{
			struct CClientKey
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NStr::CStr m_KeyID;
				CSymmetricKey m_Key;
				NContainer::TCSet<NStr::CStr> m_VerifiedOnServers;
			};

			struct CClient
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				NStr::CStr m_HostID;
				NConcurrency::TCAsyncGenerator<CClientKey> m_Keys;
			};

			struct CAvailableKeys
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				uint32 m_KeySize;
				NConcurrency::TCAsyncGenerator<CSymmetricKey> m_Keys;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NConcurrency::TCAsyncGenerator<CClient> m_Clients;
			NConcurrency::TCAsyncGenerator<CAvailableKeys> m_AvailableKeys;
		};

		struct CHostKeyID
		{
			auto operator <=> (CHostKeyID const &_Right) const noexcept = default;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStr m_HostID;
			NStr::CStr m_KeyID;
		};

		struct CUseAvailableKeyResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool m_bCanUse = false;
			// NOLINTNEXTLINE(malterlib-conceptual-prefix): a subscription handle like CActorSubscription, which the check misses because it is derived from it
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
		};

		CKeyManagerServerSync();
		~CKeyManagerServerSync();

		virtual NConcurrency::TCFuture<CReadDatabase> f_ReadDatabase() = 0;
		virtual NConcurrency::TCFuture<void> f_CreateNewKeys(NContainer::TCMap<CHostKeyID, CSymmetricKey> _Keys) = 0;
		virtual NConcurrency::TCFuture<CUseAvailableKeyResult> f_UseAvailableKey(CSymmetricKey _Key) = 0;
		virtual NConcurrency::TCFuture<void> f_PreCreateKeys(NContainer::TCSet<CSymmetricKey> _Keys) = 0;
		virtual NConcurrency::TCFuture<void> f_RemovePreCreatedKeys(NContainer::TCSet<CSymmetricKey> _Keys) = 0;
		virtual NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_RemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs, NContainer::TCSet<NStr::CStr> _CheckedServers) = 0;
		virtual NConcurrency::TCFuture<void> f_KeysVerifiedOnServers(NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> _KeysVerifiedOnServers) = 0;
	};

	struct CKeyManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/KeyManager";

		enum EProtocolVersion : uint32
		{
			EProtocolVersion_Min       = EKeyManagerProtocolVersion_Min
			, EProtocolVersion_Current = EKeyManagerProtocolVersion_Current
		};

		CKeyManager();
		~CKeyManager();

		virtual NConcurrency::TCFuture<CSymmetricKey> f_RequestKey(NStr::CStr _Identifier, uint32 _KeySize) = 0;
		// NOLINTNEXTLINE(malterlib-conceptual-prefix): a subscription handle like CActorSubscription, which the check misses because it is derived from it
		virtual auto f_GetServerSyncInterface(NConcurrency::TCActorSubscriptionWithID<> _Subscription)
			-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<CKeyManagerServerSync>> = 0
		;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_KeyManager.hpp"
