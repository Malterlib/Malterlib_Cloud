// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Storage/Variant>
#include <Mib/Storage/Optional>
#include <Mib/Encoding/EJson>
#include <Mib/Cloud/FileTransfer>
#include <Mib/File/DirectorySync>
#include <Mib/File/DirectoryManifest>

namespace NMib::NCloud
{
	DMibImpErrorClassDefine(CExceptionSecretsManagerUnexpectedValue, NException::CException);

#	define DMibErrorSecretsManagerUnexpectedValue(_Description) DMibImpError(NMib::NCloud::CExceptionSecretsManagerUnexpectedValue, _Description)
#	define DMibErrorInstanceSecretsManagerUnexpectedValue(_Description) DMibImpErrorInstance(NMib::NCloud::CExceptionSecretsManagerUnexpectedValue, _Description)

	struct CSecretsManager : public NConcurrency::CActor
	{
		static constexpr ch8 const *mc_pDefaultNamespace = "com.malterlib/Cloud/SecretsManager";

		enum : uint32
		{
			EProtocolVersion_Min = 0x101

			, EProtocolVersion_SupportImmutable = 0x106
			, EProtocolVersion_SupportNameQuery = 0x106
			, EProtocolVersion_SupportResultFlags = 0x106
			, EProtocolVersion_SupportMapSecrets = 0x106
			, EProtocolVersion_SupportExpectedMetadataAndModifiedTime = 0x106
			, EProtocolVersion_SupportOptionalDigest = 0x107

			, EProtocolVersion_Current = 0x107
		};

		enum ESecretType : int32
		{
			ESecretType_NotSet
			, ESecretType_String
			, ESecretType_Binary
			, ESecretType_File
			, ESecretType_StringMap
			, ESecretType_BinaryMap
		};

		struct CSecretID
		{

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			static CSecretID fs_Parse(NStr::CStr const &_CompoundID);

			auto operator <=> (CSecretID const &_Right) const noexcept = default;

			operator NStr::CStr() const;

			NStr::CStr m_Folder;
			NStr::CStr m_Name;
		};

		struct CSecretFile
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool operator == (CSecretFile const &_Right) const noexcept;

			NFile::CDirectoryManifestFile m_Manifest;
		};

		struct CSecret : public NStorage::TCStreamableVariant
			<
				ESecretType
				, NStorage::TCMember<void, ESecretType_NotSet>
				, NStorage::TCMember<NStr::CStrSecure, ESecretType_String>
				, NStorage::TCMember<NContainer::CSecureByteVector, ESecretType_Binary>
				, NStorage::TCMember<CSecretFile, ESecretType_File>
				, NStorage::TCMember<NContainer::TCMap<NStr::CStrSecure, NStr::CStrSecure>, ESecretType_StringMap>
				, NStorage::TCMember<NContainer::TCMap<NStr::CStrSecure, NContainer::CSecureByteVector>, ESecretType_BinaryMap>
			>
		{
			using CSuper = NStorage::TCStreamableVariant
				<
					ESecretType
					, NStorage::TCMember<void, ESecretType_NotSet>
					, NStorage::TCMember<NStr::CStrSecure, ESecretType_String>
					, NStorage::TCMember<NContainer::CSecureByteVector, ESecretType_Binary>
					, NStorage::TCMember<CSecretFile, ESecretType_File>
					, NStorage::TCMember<NContainer::TCMap<NStr::CStrSecure, NStr::CStrSecure>, ESecretType_StringMap>
					, NStorage::TCMember<NContainer::TCMap<NStr::CStrSecure, NContainer::CSecureByteVector>, ESecretType_BinaryMap>
				>
			;

			CSecret() = default;
			CSecret(CSecret const &) = default;
			CSecret(CSecret &&) = default;
			CSecret &operator = (CSecret const &) = default;
			CSecret &operator = (CSecret &&) = default;

			template <typename ...tfp_CParam>
			CSecret(tfp_CParam && ...p_Params) // This is implemented inline as MSVC seems to be broken
				: CSuper(fg_Forward<tfp_CParam>(p_Params)...)
			{
			}

			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);
			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			CSuper &operator *();
			CSuper const &operator *() const;
			bool operator == (CSecret const &_Right) const noexcept;
		};

		struct CSecretProperties
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CSecretProperties &&f_SetSecret(CSecret &&_Secret) &&;
			CSecretProperties &&f_SetUserName(NStr::CStrSecure const &_UserName) &&;
			CSecretProperties &&f_SetURL(NStr::CStrSecure const &_URL) &&;
			CSecretProperties &&f_SetExpires(NTime::CTime const &_Expires) &&;
			CSecretProperties &&f_SetNotes(NStr::CStrSecure const &_Notes) &&;
			CSecretProperties &&f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJsonSorted &&_MetadataValue) &&;
			CSecretProperties &&f_SetCreated(NTime::CTime const &_Created) &&;
			CSecretProperties &&f_SetModified(NTime::CTime const &_Modified) &&;
			CSecretProperties &&f_SetSemanticID(NStr::CStrSecure const &_SemanticID) &&;
			CSecretProperties &&f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &&;
			CSecretProperties &&f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &&;
			CSecretProperties &&f_SetImmutable() &&;

			CSecretProperties &f_SetSecret(CSecret &&_Secret) &;
			CSecretProperties &f_SetUserName(NStr::CStrSecure const &_UserName) &;
			CSecretProperties &f_SetURL(NStr::CStrSecure const &_URL) &;
			CSecretProperties &f_SetExpires(NTime::CTime const &_Expires) &;
			CSecretProperties &f_SetNotes(NStr::CStrSecure const &_Notes) &;
			CSecretProperties &f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJsonSorted &&_MetadataValue) &;
			CSecretProperties &f_SetCreated(NTime::CTime const &_Created) &;
			CSecretProperties &f_SetModified(NTime::CTime const &_Modified) &;
			CSecretProperties &f_SetSemanticID(NStr::CStrSecure const &_SemanticID) &;
			CSecretProperties &f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &;
			CSecretProperties &f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) &;
			CSecretProperties &f_SetImmutable() &;

			auto f_GetSecret() const -> CSecret const &;
			auto f_GetUserName() const -> NStr::CStrSecure const &;
			auto f_GetURL() const -> NStr::CStrSecure const &;
			auto f_GetExpires() const -> NTime::CTime const &;
			auto f_GetNotes() const -> NStr::CStrSecure const &;
			auto f_GetMetadata() const -> NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJsonSorted> const &;
			auto f_GetCreated() const -> NTime::CTime const &;
			auto f_GetModified() const -> NTime::CTime const &;
			auto f_GetSemanticID() const -> NStr::CStrSecure const &;
			auto f_GetTags() const -> NContainer::TCSet<NStr::CStrSecure> const &;
			auto f_GetImmutable() const -> bool const &;

			template <typename tf_CStr>
			void f_Format(tf_CStr &o_Str) const;

			bool operator == (CSecretProperties const &_Right) const noexcept;

			NStorage::TCOptional<CSecret> m_Secret;
			NStorage::TCOptional<NStr::CStrSecure> m_UserName;
			NStorage::TCOptional<NStr::CStrSecure> m_URL;
			NStorage::TCOptional<NTime::CTime> m_Expires;
			NStorage::TCOptional<NStr::CStrSecure> m_Notes;
			NStorage::TCOptional<NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJsonSorted>> m_Metadata;
			NStorage::TCOptional<NTime::CTime> m_Created;
			NStorage::TCOptional<NTime::CTime> m_Modified;
			NStorage::TCOptional<NStr::CStrSecure> m_SemanticID;
			NStorage::TCOptional<NContainer::TCSet<NStr::CStrSecure>> m_Tags;
			NStorage::TCOptional<bool> m_Immutable; // Does not allow the secret to be changed after being set
		};

		struct CSecretChanges
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			bool m_bFullResend = false;
			NContainer::TCMap<CSecretID, CSecretProperties> m_Changed;
			NContainer::TCSet<CSecretID> m_Removed;
		};

		struct CSubscribeToChanges
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStrSecure> m_SemanticID; //< Limit the updates to secrets matching semantic ID wildcard
			NStorage::TCOptional<NStr::CStrSecure> m_Name; //< Limit the updates to secrets matching name wildcard
			NContainer::TCSet<NStr::CStrSecure> m_TagsExclusive; //< Limit the updates to secrets with all tags specified

			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CSecretChanges _Changes)> m_fOnChanges;
		};

		struct CEnumerateSecrets
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStorage::TCOptional<NStr::CStrSecure> m_SemanticID;
			NStorage::TCOptional<NStr::CStrSecure> m_Name;
			NContainer::TCSet<NStr::CStrSecure> m_TagsExclusive;
		};

		struct CGetSecretBySemanticID
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			NStr::CStrSecure m_SemanticID;
			NStorage::TCOptional<NStr::CStrSecure> m_Name;
			NContainer::TCSet<NStr::CStrSecure> m_TagsExclusive;
		};

		enum ESetSecretPropertiesResultFlag : uint32
		{
			ESetSecretPropertiesResultFlag_None = 0
			, ESetSecretPropertiesResultFlag_Created = DMibBit(0)
			, ESetSecretPropertiesResultFlag_Updated = DMibBit(1)
		};

		struct CSetSecretPropertiesResult
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			ESetSecretPropertiesResultFlag m_Flags = ESetSecretPropertiesResultFlag_Updated; // Default for old versions of protocol
		};

		struct CSetMetadata
		{
			template <typename tf_CStream>
			void f_Stream(tf_CStream &_Stream);

			CSecretID m_ID;
			NStr::CStrSecure m_Key;
			NEncoding::CEJsonSorted m_Value;
			NStorage::TCOptional<NEncoding::CEJsonSorted> m_ExpectedValue;
			NStorage::TCOptional<NTime::CTime> m_ModifiedTime;
		};

		CSecretsManager();

		static bool fs_IsValidFolder(NStr::CStr const &_Folder);
		static bool fs_IsValidName(NStr::CStr const &_Name);
		static bool fs_IsValidNameWildcard(NStr::CStr const &_Name);
		static bool fs_IsValidTag(NStr::CStr const &_Tag);
		static bool fs_IsValidSemanticID(NStr::CStr const &_SemanticID);
		static bool fs_IsValidSemanticIDWildcard(NStr::CStr const &_SemanticID);

		virtual NConcurrency::TCFuture<NContainer::TCSet<CSecretID>> f_EnumerateSecrets(CEnumerateSecrets _Options) = 0;
		virtual NConcurrency::TCFuture<CSetSecretPropertiesResult> f_SetSecretProperties(CSecretID _ID, CSecretProperties _Secret) = 0;
		virtual NConcurrency::TCFuture<CSecretProperties> f_GetSecretProperties(CSecretID _ID) = 0;
		virtual NConcurrency::TCFuture<CSecret> f_GetSecret(CSecretID _ID) = 0;
		virtual NConcurrency::TCFuture<CSecret> f_GetSecretBySemanticID(CGetSecretBySemanticID _Options) = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCDistributedActorInterfaceWithID<NFile::CDirectorySyncClient>> f_DownloadFile
			(
				CSecretID _ID
				, NConcurrency::TCActorSubscriptionWithID<> _Subscription
			) = 0
		;
		virtual NConcurrency::TCFuture<void> f_ModifyTags
			(
				CSecretID _ID
				, NContainer::TCSet<NStr::CStrSecure> _TagsToRemove
				, NContainer::TCSet<NStr::CStrSecure> _TagsToAdd
			) = 0
		;
		virtual NConcurrency::TCFuture<void> f_SetMetadata(CSetMetadata _SetMetadata) = 0;
		virtual NConcurrency::TCFuture<void> f_RemoveMetadata(CSecretID _ID, NStr::CStrSecure _MetadataKey) = 0;
		virtual NConcurrency::TCFuture<void> f_RemoveSecret(CSecretID _ID) = 0;
		virtual NConcurrency::TCFuture<NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> ()>> f_UploadFile
			(
				CSecretID _ID
				, NStr::CStrSecure _FileName
				, NConcurrency::TCDistributedActorInterfaceWithID<NFile::CDirectorySyncClient> _Uploader
			) = 0
		;
		virtual NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>> f_SubscribeToChanges(CSubscribeToChanges _Params) = 0;
	};
}

#ifndef DMibPNoShortCuts
	using namespace NMib::NCloud;
#endif

#include "Malterlib_Cloud_SecretsManager.hpp"
