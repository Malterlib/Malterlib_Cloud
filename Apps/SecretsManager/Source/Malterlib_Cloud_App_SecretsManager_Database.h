// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/SecretsManager>

#include "Malterlib_Cloud_App_SecretsManager.h"

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretPropertiesInternal
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
		void f_Format(CStrAggregate &o_Str) const;
		CSecretsManager::CSecretID const &f_GetSecretID() const;
		CSecretsManager::CSecretProperties f_ToSecretProperties() const;

		CSecretsManager::CSecret m_Secret;
		NStr::CStrSecure m_UserName;
		NStr::CStrSecure m_URL;
		NTime::CTime m_Expires;
		NStr::CStrSecure m_Notes;
		NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSONSorted> m_Metadata;
		NTime::CTime m_Created;
		NTime::CTime m_Modified;
		NStr::CStrSecure m_SemanticID;
		NContainer::TCSet<NStr::CStrSecure> m_Tags;
		NContainer::CSecureByteVector m_Key;
		NContainer::CSecureByteVector m_IV;
		NContainer::CSecureByteVector m_HMACKey;
		NStr::CStr m_RandomFileName;
		bool m_bImmutable = false;
	};

	struct CSecretsDatabase
	{
		enum
		{
			ESecretsManagerDatabaseVersion = 0x102
		};

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> m_Secrets;
	};

	struct CSecretsDatabaseIV
	{
		enum
		{
			ESecretsManagerDatabaseIVVersion = 0x101
		};

		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);

		uint64 m_InternalSalt = 0;
		uint64 m_ExternalSalt = 0;
	};


	class CSecretsManagerServerDatabase : public CActor
	{
	public:
		CSecretsManagerServerDatabase(NStr::CStr const &_Path, NContainer::CSecureByteVector const &_Key);
		~CSecretsManagerServerDatabase();
		
		NConcurrency::TCFuture<void> f_Initialize();
		NConcurrency::TCFuture<void> f_WriteDatabase(CSecretsDatabase &&_Database);
		NConcurrency::TCFuture<CSecretsDatabase> f_ReadDatabase();

	private:
		struct CEncryptionState
		{
			NContainer::CSecureByteVector const m_Key;
			CSecretsDatabaseIV m_IVSalt;
		};

		static CSecretsDatabaseIV fsp_WriteDatabase(CSecretsDatabase const &_Database, CStr const &_Path, CEncryptionState const &_State);
		static CSecretsDatabaseIV fsp_ReadDatabase(CSecretsDatabase *_pDatabase, CStr const &_Path, CEncryptionState const &_State);
		TCFuture<void> fp_Destroy() override;
		static NContainer::CSecureByteVector fsp_ComputeSalt(CSecretsDatabaseIV const &_Salt);

		TCSharedPointer<CSecretsDatabase> mp_pPendingWrite;
		TCVector<TCPromise<void>> mp_PendingWritePromises;
		CEncryptionState mp_EncryptionState;
		NStr::CStr mp_Path;
		CSequencer mp_Sequencer{"SecretsManagerDatabase"};
	};
}

#include "Malterlib_Cloud_App_SecretsManager_Database.hpp"

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif
