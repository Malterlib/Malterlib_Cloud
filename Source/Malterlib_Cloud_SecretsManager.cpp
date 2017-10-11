// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_SecretsManager.h"

namespace NMib::NCloud
{
	using namespace NStr;
	
	CSecretsManager::CSecretsManager()
	{
		DMibPublishActorFunction(CSecretsManager::f_EnumerateSecrets);
		DMibPublishActorFunction(CSecretsManager::f_SetSecretProperties);
		DMibPublishActorFunction(CSecretsManager::f_GetSecretProperties);
		DMibPublishActorFunction(CSecretsManager::f_GetSecret);
		DMibPublishActorFunction(CSecretsManager::f_GetSecretBySemanticID);
		DMibPublishActorFunction(CSecretsManager::f_DownloadFile);
		DMibPublishActorFunction(CSecretsManager::f_ModifyTags);
		DMibPublishActorFunction(CSecretsManager::f_SetMetadata);
		DMibPublishActorFunction(CSecretsManager::f_RemoveMetadata);
		DMibPublishActorFunction(CSecretsManager::f_UploadFile);
	}

	auto CSecretsManager::CSecretProperties::f_Secret(CSecret &&_Secret) && -> CSecretProperties &&
	{
		m_Secret = fg_Move(_Secret);
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_UserName(NStr::CStrSecure const &_UserName) && -> CSecretProperties &&
	{
		m_UserName = _UserName;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_URL(NStr::CStrSecure const &_URL) && -> CSecretProperties &&
	{
		m_URL = _URL;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Expires(NTime::CTime const &_Expires) && -> CSecretProperties &&
	{
		m_Expires = _Expires;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Notes(NStr::CStrSecure const &_Notes) && -> CSecretProperties &&
	{
		m_Notes = _Notes;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Metadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_MetadataValue) && -> CSecretProperties &&
	{
		if (!m_Metadata)
			m_Metadata = NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON>{};
		(*m_Metadata)[_MetadataKey] = _MetadataValue;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Created(NTime::CTime const &_Created) && -> CSecretProperties &&
	{
		m_Created = _Created;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Modified(NTime::CTime const &_Modified) && -> CSecretProperties &&
	{
		m_Modified = _Modified;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SemanticID(NStr::CStrSecure const &_SemanticID) && -> CSecretProperties &&
	{
		m_SemanticID = _SemanticID;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_Tags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) && -> CSecretProperties &&
	{
		m_Tags = fg_Move(_Tags);
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) && -> CSecretProperties &&
	{
		if (!m_Tags)
			m_Tags = NContainer::TCSet<NStr::CStrSecure>{};
		*m_Tags += fg_Move(_Tags);
		return fg_Move(*this);
	}

	namespace NPrivate
	{

		struct CSecretEqualsVisitor
		{
			template <typename tf_CTypeLeft, typename tf_CTypeRight>
			bool operator ()(tf_CTypeLeft const &, tf_CTypeRight const &)
			{
				return false;
			}

			template <typename tf_CType>
			bool operator ()(tf_CType const &_Left, tf_CType const &_Right)
			{
				return _Left == _Right;
			}
		};
	};
	
	bool CSecretsManager::CSecret::operator == (CSecret const &_Right) const
	{
		return fg_VisitRet<bool>(NPrivate::CSecretEqualsVisitor(), *this, _Right);
	}
}
