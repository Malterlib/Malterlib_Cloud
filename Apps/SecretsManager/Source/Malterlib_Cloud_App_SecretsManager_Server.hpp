// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud
{
	template <typename tf_CStream>
	void NSecretsManager::CSecretPropertiesInternal::f_Stream(tf_CStream &_Stream)
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
}
