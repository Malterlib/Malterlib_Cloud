// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Storage/Tuple>

namespace NMib::NCloud
{
	inline bool CSecretsManager::CSecretID::operator < (CSecretID const &_Right) const
	{
		return NContainer::fg_TupleReferences(m_Folder, m_Name) < NContainer::fg_TupleReferences(_Right.m_Folder, _Right.m_Name);
	}
	inline bool CSecretsManager::CSecretID::operator == (CSecretID const &_Right) const
	{
		return NContainer::fg_TupleReferences(m_Folder, m_Name) == NContainer::fg_TupleReferences(_Right.m_Folder, _Right.m_Name);
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
	void CSecretsManager::CFileTag::f_Stream(tf_CStream &_Stream)
	{
	}

	inline bool CSecretsManager::CFileTag::operator == (CSecretsManager::CFileTag const &_Right) const
	{
		return true;
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
			o_Str += NDataProcessing::fg_Base64Encode(f_Get<CSecretsManager::ESecretType_Binary>());
			break;

		case CSecretsManager::ESecretType_File:
			o_Str += "<File Secret>";
			break;
		}
	}
}
