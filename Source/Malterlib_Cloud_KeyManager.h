// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/AsyncGenerator>

#include "Malterlib_Cloud_KeyManager_Shared.h"

namespace NMib::NCloud
{
	enum : uint32
	{
		EKeyManagerProtocolVersion_Min = 0x101
		, EKeyManagerProtocolVersion_SupportServerSync = 0x102

		, EKeyManagerProtocolVersion_Current = 0x102
	};

	struct CKeyManagerServerSync : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/KeyManager/ServerSync";

		enum : uint32
		{
			EProtocolVersion_Min = EKeyManagerProtocolVersion_Min
			, EProtocolVersion_Current = EKeyManagerProtocolVersion_Current
		};

		struct CReadDatabase
		{
			struct CClientKey
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream)
				{
					_Stream % m_KeyID;
					_Stream % m_Key;
					_Stream % m_VerifiedOnServers;
				}

				NStr::CStr m_KeyID;
				CSymmetricKey m_Key;
				NContainer::TCSet<NStr::CStr> m_VerifiedOnServers;
			};

			struct CClient
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream)
				{
					_Stream % m_HostID;
					_Stream % m_Keys;
				}

				NStr::CStr m_HostID;
				NConcurrency::TCAsyncGenerator<CClientKey> m_Keys;
			};

			struct CAvailableKeys
			{
				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream)
				{
					_Stream % m_KeySize;
					_Stream % m_Keys;
				}

				uint32 m_KeySize;
				NConcurrency::TCAsyncGenerator<CSymmetricKey> m_Keys;
			};

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream)
			{
				_Stream % m_Clients;
				_Stream % m_AvailableKeys;
			}

			NConcurrency::TCAsyncGenerator<CClient> m_Clients;
			NConcurrency::TCAsyncGenerator<CAvailableKeys> m_AvailableKeys;
		};

		struct CHostKeyID
		{
			auto operator <=> (CHostKeyID const &) const = default;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream)
			{
				_Stream % m_HostID;
				_Stream % m_KeyID;
			}

			NStr::CStr m_HostID;
			NStr::CStr m_KeyID;
		};

		struct CUseAvailableKeyResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream)
			{
				_Stream % m_bCanUse;
				_Stream % fg_Move(m_Subscription);
			}

			bool m_bCanUse = false;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
		};

		CKeyManagerServerSync();
		~CKeyManagerServerSync();
		
		virtual NConcurrency::TCFuture<CReadDatabase> f_ReadDatabase() = 0;
		virtual NConcurrency::TCFuture<void> f_CreateNewKeys(NContainer::TCMap<CHostKeyID, CSymmetricKey> &&_Keys) = 0;
		virtual NConcurrency::TCFuture<CUseAvailableKeyResult> f_UseAvailableKey(CSymmetricKey &&_Key) = 0;
		virtual NConcurrency::TCFuture<void> f_PreCreateKeys(NContainer::TCSet<CSymmetricKey> &&_Keys) = 0;
		virtual NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_RemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> &&_HostIDs, NContainer::TCSet<NStr::CStr> &&_CheckedServers) = 0;
		virtual NConcurrency::TCFuture<void> f_KeysVerifiedOnServers(NContainer::TCMap<CKeyManagerServerSync::CHostKeyID, NContainer::TCSet<NStr::CStr>> &&_KeysVerifiedOnServers) = 0;
	};

	struct CKeyManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/KeyManager";
		
		enum : uint32
		{
			EProtocolVersion_Min = EKeyManagerProtocolVersion_Min
			, EProtocolVersion_Current = EKeyManagerProtocolVersion_Current
		};

		CKeyManager();
		~CKeyManager();
		
		virtual NConcurrency::TCFuture<CSymmetricKey> f_RequestKey(NStr::CStr const &_Identifier, uint32 _KeySize) = 0;
		virtual auto f_GetServerSyncInterface(NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
			-> NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<CKeyManagerServerSync>>
		= 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
