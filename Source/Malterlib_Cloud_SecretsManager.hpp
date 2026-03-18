// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Storage/Tuple>

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void CSecretsManager::CSecretID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Folder;
		_Stream % m_Name;
	}

	template <typename tf_CStr>
	void CSecretsManager::CSecretID::f_Format(tf_CStr &o_Str) const
	{
		o_Str += typename tf_CStr::CFormat("{}/{}") << m_Folder << m_Name;
	}

	inline CSecretsManager::CSecretID::operator NStr::CStr() const
	{
		return NStr::CStr::CFormat("{}/{}") << m_Folder << m_Name;
	}


	template <typename tf_CStream>
	void CSecretsManager::CSecretProperties::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Secret;
		_Stream % m_UserName;
		_Stream % m_URL;
		_Stream % m_Expires;
		_Stream % m_Notes;
		_Stream % m_Metadata;
		_Stream % m_Created;
		_Stream % m_Modified;
		_Stream % m_SemanticID;
		_Stream % m_Tags;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportImmutable)
			_Stream % m_Immutable;
	}

	template <typename tf_CStream>
	void CSecretsManager::CSecretFile::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = NFile::CDirectoryManifest::EManifestStreamVersion_Min;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportOptionalDigest)
			Version = NFile::CDirectoryManifest::EManifestStreamVersion_OptionalDigest;

		DMibBinaryStreamVersion(_Stream, Version);
		m_Manifest.f_Stream(_Stream, Version);
	}

	inline bool CSecretsManager::CSecretFile::operator == (CSecretsManager::CSecretFile const &_Right) const noexcept
	{
		return m_Manifest == _Right.m_Manifest;
	}

	inline auto CSecretsManager::CSecret::operator *() -> CSuper &
	{
		return *this;
	}

	inline auto CSecretsManager::CSecret::operator *() const -> CSuper const &
	{
		return *this;
	}

	template <typename tf_CStream>
	void CSecretsManager::CSecret::f_Stream(tf_CStream &_Stream)
	{
		_Stream % **this;
	}

	template <typename tf_CStr>
	void CSecretsManager::CSecret::f_Format(tf_CStr &o_Str) const
	{
		switch (f_GetTypeID())
		{
		case CSecretsManager::ESecretType_NotSet:
			o_Str += "<Empty Secret>";
			break;

		case CSecretsManager::ESecretType_String:
			o_Str += f_Get<CSecretsManager::ESecretType_String>();
			break;

		case CSecretsManager::ESecretType_Binary:
			o_Str += NEncoding::fg_Base64Encode(f_Get<CSecretsManager::ESecretType_Binary>());
			break;

		case CSecretsManager::ESecretType_File:
			o_Str += f_Get<CSecretsManager::ESecretType_File>().m_Manifest.m_OriginalPath;
			break;

		case CSecretsManager::ESecretType_StringMap:
			{
				auto &StringMap = f_Get<CSecretsManager::ESecretType_StringMap>();
				for (auto &Value : StringMap)
				{
					auto &Key = StringMap.fs_GetKey(Value);
					o_Str += typename tf_CStr::CFormat("{}: {}\n") << Key << Value;
				}
			}
			break;

		case CSecretsManager::ESecretType_BinaryMap:
			{
				auto &StringMap = f_Get<CSecretsManager::ESecretType_BinaryMap>();
				for (auto &Value : StringMap)
				{
					auto &Key = StringMap.fs_GetKey(Value);
					o_Str += typename tf_CStr::CFormat("{}: {}\n") << Key << NEncoding::fg_Base64Encode(Value);
				}
			}
			break;
		}
	}

	template <typename tf_CStr>
	void CSecretsManager::CSecretProperties::f_Format(tf_CStr &o_Str) const
	{
		if (m_Secret)
			o_Str += typename tf_CStr::CFormat("Secret: {}\n") << *m_Secret;
		if (m_UserName)
			o_Str += typename tf_CStr::CFormat("UserName: {}\n") << *m_UserName;
		if (m_URL)
			o_Str += typename tf_CStr::CFormat("URL: {}\n") << *m_URL;
		if (m_Expires)
			o_Str += typename tf_CStr::CFormat("Expires: {}\n") << *m_Expires;
		if (m_Notes)
			o_Str += typename tf_CStr::CFormat("Notes: {}\n") << *m_Notes;
		if (m_Metadata)
			o_Str += typename tf_CStr::CFormat("Metadata: {}\n") << *m_Metadata;
		if (m_Created)
			o_Str += typename tf_CStr::CFormat("Created: {}\n") << *m_Created;
		if (m_Modified)
			o_Str += typename tf_CStr::CFormat("Modified: {}\n") << *m_Modified;
		if (m_SemanticID)
			o_Str += typename tf_CStr::CFormat("SemanticID: {}\n") << *m_SemanticID;
		if (m_Tags)
			o_Str += typename tf_CStr::CFormat("Tags: {vs}\n") << *m_Tags;
		if (m_Immutable)
			o_Str += typename tf_CStr::CFormat("Immutable: {}\n") << (*m_Immutable ? "true" : "false");
	}

	template <typename tf_CStream>
	void CSecretsManager::CSecretChanges::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_bFullResend;
		_Stream % m_Changed;
		_Stream % m_Removed;
	}

	template <typename tf_CStream>
	void CSecretsManager::CSubscribeToChanges::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_SemanticID;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportNameQuery)
			_Stream % m_Name;
		_Stream % m_TagsExclusive;

		_Stream % fg_Move(m_fOnChanges);
	}

	template <typename tf_CStream>
	void CSecretsManager::CEnumerateSecrets::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_SemanticID;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportNameQuery)
			_Stream % m_Name;
		_Stream % m_TagsExclusive;
	}

	template <typename tf_CStream>
	void CSecretsManager::CGetSecretBySemanticID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_SemanticID;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportNameQuery)
			_Stream % m_Name;
		_Stream % m_TagsExclusive;
	}

	template <typename tf_CStream>
	void CSecretsManager::CSetSecretPropertiesResult::f_Stream(tf_CStream &_Stream)
	{
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportResultFlags)
			_Stream % m_Flags;
	}

	template <typename tf_CStream>
	void CSecretsManager::CSetMetadata::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_ID;
		_Stream % m_Key;
		_Stream % m_Value;
		if (_Stream.f_GetVersion() >= EProtocolVersion_SupportExpectedMetadataAndModifiedTime)
		{
			_Stream % m_ExpectedValue;
			_Stream % m_ModifiedTime;
		}
	}
}
