// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Network/SSL>
#include <Mib/Cloud/SecretsManager>

#include "Malterlib_Cloud_App_SecretsManager.h"

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretPropertiesInternal
	{
		template <typename tf_CStream>
		void f_Stream(tf_CStream &_Stream);
		void f_Format(CStrAggregate &o_Str) const;
		
		CSecretsManager::CSecret m_Secret;
		NStr::CStrSecure m_UserName;
		NStr::CStrSecure m_URL;
		NTime::CTime m_Expires;
		NStr::CStrSecure m_Notes;
		NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON> m_Metadata;
		NTime::CTime m_Created;
		NTime::CTime m_Modified;
		NStr::CStrSecure m_SemanticID;
		NContainer::TCSet<NStr::CStrSecure> m_Tags;
		NContainer::CSecureByteVector m_Key;
		NContainer::CSecureByteVector m_IV;
		NContainer::CSecureByteVector m_HMACKey;
		NStr::CStr m_RandomFileName;
	};

	struct CSecretsDatabase
	{
		enum
		{
			ESecretsManagerDatabaseVersion = 0x101
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
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;

		CSecretsManagerServerDatabase(NStr::CStr const &_Path, NContainer::CSecureByteVector const &_Key);
		~CSecretsManagerServerDatabase();
		
		NConcurrency::TCContinuation<void> f_Initialize();
		NConcurrency::TCContinuation<void> f_WriteDatabase(CSecretsDatabase &&_Database);
		NConcurrency::TCContinuation<CSecretsDatabase> f_ReadDatabase();

	private:
		void fp_WriteDatabase(CSecretsDatabase const &_Database);
		void fp_ReadDatabase(CSecretsDatabase *_pDatabase);
		TCContinuation<void> fp_Destroy() override;
		NContainer::CSecureByteVector fp_ComputeSalt(CSecretsDatabaseIV const &_Salt);

		TCSharedPointer<CSecretsDatabase> mp_pPendingWrite;
		TCVector<TCContinuation<void>> mp_PendingWriteContinuations;
		NStr::CStr mp_Path;
		NContainer::CSecureByteVector const mp_Key;
		CSecretsDatabaseIV mp_IVSalt;
	};
}

#include "Malterlib_Cloud_App_SecretsManager_Database.hpp"

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
