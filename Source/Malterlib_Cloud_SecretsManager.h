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
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/SecretsManager";
		
		CSecretsManager();

		enum : uint32
		{
			EMinProtocolVersion = 0x101
			, EProtocolVersion = 0x101
		};
		
		enum ESecretType : int32
		{
			ESecretType_NotSet
			, ESecretType_String
			, ESecretType_Buffer
			, ESecretType_File
		};
		
		struct CSecretID
		{
			bool operator < (CSecretID const &_Right) const;
			bool operator == (CSecretID const &_Right) const;
			
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			NStr::CStr m_Folder;
			NStr::CStr m_Name;
		};
		
		struct CFileTag
		{
			bool operator == (CFileTag const &_Right) const;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
		};
		
		struct CSecret : public NContainer::TCStreamableVariant
			<
				ESecretType
				, void, ESecretType_NotSet
				, NStr::CStrSecure, ESecretType_String
				, NContainer::CSecureByteVector, ESecretType_Buffer
				, CFileTag, ESecretType_File
			>
		{
			using CSuper = NContainer::TCStreamableVariant
				<
					ESecretType
					, void, ESecretType_NotSet
					, NStr::CStrSecure, ESecretType_String
					, NContainer::CSecureByteVector, ESecretType_Buffer
					, CFileTag, ESecretType_File
				>
			;
			
			CSuper &operator *();
			CSuper const &operator *() const;
			bool operator == (CSecret const &_Right) const;

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;
		};

		struct CSecretProperties
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

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
			
			CSecretProperties &&f_SetSecret(CSecret &&_Secret) &&;
			CSecretProperties &&f_SetUserName(NStr::CStrSecure const &_UserName) &&;
			CSecretProperties &&f_SetURL(NStr::CStrSecure const &_URL) &&;
			CSecretProperties &&f_SetExpires(NTime::CTime const &_Expires) &&;
			CSecretProperties &&f_SetNotes(NStr::CStrSecure const &_Notes) &&;
			CSecretProperties &&f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_MetadataValue) &&;
			CSecretProperties &&f_SetCreated(NTime::CTime const &_Created) &&;
			CSecretProperties &&f_SetModified(NTime::CTime const &_Modified) &&;
			CSecretProperties &&f_SetSemanticID(NStr::CStrSecure const &_SemanticID) &&;
			CSecretProperties &&f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &&;
			CSecretProperties &&f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &&;

			CSecretProperties &f_SetSecret(CSecret &&_Secret) &;
			CSecretProperties &f_SetUserName(NStr::CStrSecure const &_UserName) &;
			CSecretProperties &f_SetURL(NStr::CStrSecure const &_URL) &;
			CSecretProperties &f_SetExpires(NTime::CTime const &_Expires) &;
			CSecretProperties &f_SetNotes(NStr::CStrSecure const &_Notes) &;
			CSecretProperties &f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_MetadataValue) &;
			CSecretProperties &f_SetCreated(NTime::CTime const &_Created) &;
			CSecretProperties &f_SetModified(NTime::CTime const &_Modified) &;
			CSecretProperties &f_SetSemanticID(NStr::CStrSecure const &_SemanticID) &;
			CSecretProperties &f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &;
			CSecretProperties &f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &;

			auto f_GetSecret() const -> CSecret const &;
			auto f_GetUserName() const -> NStr::CStrSecure const &;
			auto f_GetURL() const -> NStr::CStrSecure const &;
			auto f_GetExpires() const -> NTime::CTime const &;
			auto f_GetNotes() const -> NStr::CStrSecure const &;
			auto f_GetMetadata() const -> NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON> const &;
			auto f_GetCreated() const -> NTime::CTime const &;
			auto f_GetModified() const -> NTime::CTime const &;
			auto f_GetSemanticID() const -> NStr::CStrSecure const &;
			auto f_GetTags() const -> NContainer::TCSet<NStr::CStrSecure> const &;

			bool operator == (CSecretProperties const &_Right) const;
		};

		static bool fs_IsValidTag(NStr::CStr const &_Tag);

		virtual NConcurrency::TCContinuation<NContainer::TCSet<CSecretID>> f_EnumerateSecrets
			(
				NStorage::TCOptional<NStr::CStrSecure> &_SemanticID
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
