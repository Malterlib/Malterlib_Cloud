// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Storage/Tuple>

namespace NMib::NCloud
{
	inline bool CSecretsManager::CSecretID::operator < (CSecretID const &_Right) const
	{
		return NStorage::fg_TupleReferences(m_Folder, m_Name) < NStorage::fg_TupleReferences(_Right.m_Folder, _Right.m_Name);
	}
	inline bool CSecretsManager::CSecretID::operator == (CSecretID const &_Right) const
	{
		return NStorage::fg_TupleReferences(m_Folder, m_Name) == NStorage::fg_TupleReferences(_Right.m_Folder, _Right.m_Name);
	}

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
	}

	template <typename tf_CStream>
	void CSecretsManager::CSecretFile::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = 0x102;
		DMibBinaryStreamVersion(_Stream, 0x102);
		m_Manifest.f_Stream(_Stream, Version);
	}

	inline bool CSecretsManager::CSecretFile::operator == (CSecretsManager::CSecretFile const &_Right) const
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
		}
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
		_Stream % m_TagsExclusive;

		_Stream % fg_Move(m_fOnChanges);
	}
}
