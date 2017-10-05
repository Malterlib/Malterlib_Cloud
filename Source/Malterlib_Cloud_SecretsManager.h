// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Storage/Variant>
#include <Mib/Storage/Optional>
#include <Mib/Encoding/EJSON>
#include <Mib/Cloud/FileTransfer>

namespace NMib::NCloud
{
	DMibImpErrorClass(CExceptionSecrets, NException::CException);
		
#	define DMibErrorSecrets(_Description) DMibImpError(NMib::NCloud::CExceptionSecrets, _Description)

#	ifndef DMibPNoShortCuts
#		define DErrorSecrets(_Description) DMibErrorSecrets(_Description)
#	endif
	
	struct CSecretsManager : public NConcurrency::CActor
	{
		CSecretsManager();

		enum : uint32
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};
		
		enum ESecretType : int32
		{
			
			ESecretType_String
			, ESecretType_Buffer
			, ESecretType_File
		};
		
		struct CSecretID
		{
			NStr::CStr m_Folder;
			NStr::CStr m_Name;
			
			bool operator < (CSecretID const &_Right) const;
			bool operator == (CSecretID const &_Right) const;
			
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

		};
		
		struct CFileTag
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};
		
		using CSecret = NContainer::TCStreamableVariant
			<
				ESecretType
				, NStr::CStrSecure, ESecretType_String
				, NContainer::CSecureByteVector, ESecretType_Buffer
				, CFileTag, ESecretType_File
			>
		;

		struct CSecretProperties
		{
			NStorage::TCOptional<CSecret> m_Secret;
			NStorage::TCOptional<NStr::CStrSecure> m_UserName;
			NStorage::TCOptional<NStr::CStrSecure> m_URL;
			NStorage::TCOptional<NTime::CTime> m_Expires;
			NStorage::TCOptional<NStr::CStrSecure> m_Notes;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON>> m_Metadata;
			NStorage::TCOptional<NTime::CTime> m_Created;
			NStorage::TCOptional<NTime::CTime> m_Modified;
			NStorage::TCOptional<NStr::CStrSecure> m_SemanticID;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStrSecure>> m_Tags;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);			
		};

		virtual NConcurrency::TCContinuation<NContainer::TCSet<CSecretID>> f_EnumerateSecrets
			(
				NStorage::TCOptional<NStr::CStrSecure const> &_SemanticID
				, NContainer::TCSet<NStr::CStrSecure> const &_TagsExclusive
			 ) = 0
		;
		virtual NConcurrency::TCContinuation<void> f_SetSecretProperties(CSecretID &&_ID, CSecretProperties &&_Secret) = 0;
		virtual NConcurrency::TCContinuation<CSecretProperties> f_GetSecretProperties(CSecretID &&_ID) = 0;
		virtual NConcurrency::TCContinuation<CSecret> f_GetSecret(CSecretID &&_ID) = 0;
		virtual NConcurrency::TCContinuation<CSecret> f_GetSecretBySemanticID(NStr::CStrSecure const &_SemanticID, NContainer::TCSet<NStr::CStrSecure> const &_TagsExclusive) = 0;
		virtual NConcurrency::TCContinuation<NConcurrency::CActorSubscription> f_DownloadFile(CFileTransferContext &&_TransferContext) = 0;
		virtual NConcurrency::TCContinuation<void> f_ModifyTags
			(
				CSecretID &&_ID
				, NContainer::TCSet<NStr::CStrSecure> &&_TagsToRemove
				, NContainer::TCSet<NStr::CStrSecure> &&_TagsToAdd
			) = 0
		;
		virtual NConcurrency::TCContinuation<void> f_SetMetadata(CSecretID &&_ID, NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_Metadata) = 0;
		virtual NConcurrency::TCContinuation<void> f_RemoveMetadata(CSecretID &&_ID, NStr::CStrSecure const &_MetadataKey) = 0;
		virtual auto f_UploadFile(NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<void> (CFileTransferContext &&_TransferContext)> &&_fOnNotification)
			-> NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> = 0;
		;
	};
}

#ifndef DMibPNoShortCuts
using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_SecretsManager.hpp"
