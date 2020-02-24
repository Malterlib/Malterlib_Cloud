// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

namespace NMib::NCloud::NSecretsManager
{
	template <typename tf_CStream>
	void CSecretPropertiesInternal::f_Stream(tf_CStream &_Stream)
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
		_Stream % m_Key;
		_Stream % m_IV;
		_Stream % m_HMACKey;
		_Stream % m_RandomFileName;
	}

	template <typename tf_CStream>
	void CSecretsDatabase::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = ESecretsManagerDatabaseVersion;
		_Stream % Version;
		if (Version < 0x101 || Version > ESecretsManagerDatabaseVersion)
			DMibError("Invalid secrets database version");
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_Secrets;
	}

	template <typename tf_CStream>
	void CSecretsDatabaseIV::f_Stream(tf_CStream &_Stream)
	{
		uint32 Version = ESecretsManagerDatabaseIVVersion;
		_Stream % Version;
		if (Version < 0x101 || Version > ESecretsManagerDatabaseIVVersion)
			DMibError("Invalid secrets database IV version");
		DMibBinaryStreamVersion(_Stream, Version);

		_Stream % m_InternalSalt;
		_Stream % m_ExternalSalt;
	}
	
	inline CSecretsManager::CSecretID const &CSecretPropertiesInternal::f_GetSecretID() const
	{
		return TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal>::fs_GetKey(*this);
	}
}
