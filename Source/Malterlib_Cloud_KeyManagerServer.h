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
				mc_ServerSyncSupport = 0x101

				, mc_Current = 0x101
			};

			struct CClientKey
			{
				auto operator <=> (CClientKey const &) const = default;

				NStr::CStr const &f_GetID() const;

				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				template <typename tf_CStr>
				void f_Format(tf_CStr &o_Str) const
				{
					o_Str += typename tf_CStr::CFormat("Key: {}   Verified: {vs}") << m_Key << m_VerifiedOnServers;
				}

				CSymmetricKey m_Key;
				NContainer::TCSet<NStr::CStr> m_VerifiedOnServers;
			};

			struct CClientStore
			{
				auto operator <=> (CClientStore const &) const = default;

				NStr::CStr const &f_GetID() const;

				template <typename tf_CStream>
				void f_Stream(tf_CStream &_Stream);

				template <typename tf_CStr>
				void f_Format(tf_CStr &o_Str) const
				{
					o_Str += typename tf_CStr::CFormat("{}") << m_Keys;
				}

				NContainer::TCMap<NStr::CStr, CClientKey> m_Keys;
			};

			auto operator <=> (CDatabase const &) const = default;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool f_HasAvailableKey(CSymmetricKey _Key);

			NContainer::TCMap<NStr::CStr, CClientStore> m_Clients;
			NContainer::TCMap<uint32, NContainer::TCSet<CSymmetricKey>> m_AvailableKeys;
		};

		virtual NConcurrency::TCFuture<void> f_Initialize() = 0;
		virtual NConcurrency::TCFuture<void> f_ChangePassword(NStr::CStrSecure _Password, NContainer::CSecureByteVector _Salt) = 0;
		virtual NConcurrency::TCFuture<void> f_WriteDatabase(CDatabase _Database) = 0;
		virtual NConcurrency::TCFuture<CDatabase> f_ReadDatabase() = 0;
	};

	struct CKeyManagerServerConfig
	{
		NConcurrency::TCActor<ICKeyManagerServerDatabase> m_DatabaseActor;
		NConcurrency::TCActor<NConcurrency::CDistributedActorTrustManager> m_TrustManager;
		NFunction::TCFunctionMovable<NConcurrency::CDistributedAppAuditor (NConcurrency::CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category)> m_fAuditorFactory
			= [](NConcurrency::CCallingHostInfo const &_CallingHostInfo, NStr::CStr const &_Category) -> NConcurrency::CDistributedAppAuditor
			{
				return NConcurrency::CDistributedAppAuditor({}, _CallingHostInfo, _Category);
			}
		;
		uint32 m_CreateNewKeyMinServers = 1;
	};

	struct CKeyManagerServer : public NConcurrency::CActor
	{
		struct CKeyManagerKeyID
		{
			NStr::CStr m_HostID;
			NStr::CStr m_KeyID;
		};

		struct CKeyManagerKey
		{
			CKeyManagerKeyID m_Key;
			NContainer::TCSet<NStr::CStr> m_VerifiedOnServers;
		};

		CKeyManagerServer(CKeyManagerServerConfig &&_Config);
		~CKeyManagerServer();

		NConcurrency::TCFuture<void> f_Init(fp32 _WaitForPublicationsTimeout);
		NConcurrency::TCFuture<void> f_PreCreateKeys(uint32 _KeySize, uint32 _nKeys);
		NConcurrency::TCFuture<uint32> f_RemovePreCreatedKeys(NStorage::TCOptional<uint32> _KeySize);
		NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_RemoveVerifiedHosts(NContainer::TCSet<NStr::CStr> _HostIDs);
		NConcurrency::TCFuture<NContainer::TCSet<NStr::CStr>> f_GetAllVerifiedHosts();
		NConcurrency::TCFuture<NContainer::TCVector<CKeyManagerKey>> f_GetKeys();
		NConcurrency::TCFuture<NContainer::TCMap<uint32, uint32>> f_GetPreCreatedKeysStats();
		NConcurrency::TCFuture<void> f_CopyKey(CKeyManagerKeyID _FromKey, CKeyManagerKeyID _ToKey);
		NConcurrency::TCFuture<void> f_ForceWriteDatabase();

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
