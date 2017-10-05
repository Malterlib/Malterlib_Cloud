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
	void NMib::NCloud::CSecretsManager::CSecretID::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Folder;
		_Stream % m_Name;
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
}
