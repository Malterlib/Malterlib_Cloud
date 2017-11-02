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
	};
	
	class CSecretsManagerServerDatabase : public CActor
	{
	public:
		using CActorHolder = NConcurrency::CSeparateThreadActorHolder;
		using CDatabase = TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal>;
		
		CSecretsManagerServerDatabase(NStr::CStr const &_Path, NContainer::CSecureByteVector const &_Key, NNet::CEncryptAES::CSalt const *_pSalt);
		~CSecretsManagerServerDatabase();
		
		NConcurrency::TCContinuation<void> f_Initialize();
		NConcurrency::TCContinuation<void> f_WriteDatabase(CSecretsManagerServerDatabase::CDatabase &&_Database);
		NConcurrency::TCContinuation<CSecretsManagerServerDatabase::CDatabase> f_ReadDatabase();

	private:
		void fp_WriteDatabase(CDatabase const &_Database);
		TCContinuation<void> fp_Destroy() override;

		TCSharedPointer<CDatabase> mp_pPendingWrite;
		TCVector<TCContinuation<void>> mp_PendingWriteContinuations;
		NNet::CEncryptAES mp_AESContext;
		NStr::CStr mp_Path;
		static constexpr ch8 const *mc_pPlainTextCheck = "ABCDEFGHIJKLMNOPQRSTUVWXYZABCDEF";
	};
}

#include "Malterlib_Cloud_App_SecretsManager_Database.hpp"

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif
