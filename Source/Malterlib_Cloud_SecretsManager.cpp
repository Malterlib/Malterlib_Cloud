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
		DMibPublishActorFunction(CSecretsManager::f_RemoveSecret);
		DMibPublishActorFunction(CSecretsManager::f_UploadFile);
	}

	bool CSecretsManager::fs_IsValidTag(CStr const &_Tag)
	{
		return NNet::fg_IsValidHostname(_Tag);
	}

	auto CSecretsManager::CSecretProperties::f_SetSecret(CSecret &&_Secret) && -> CSecretProperties &&
	{
		m_Secret = fg_Move(_Secret);
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetUserName(NStr::CStrSecure const &_UserName) && -> CSecretProperties &&
	{
		m_UserName = _UserName;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetURL(NStr::CStrSecure const &_URL) && -> CSecretProperties &&
	{
		m_URL = _URL;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetExpires(NTime::CTime const &_Expires) && -> CSecretProperties &&
	{
		m_Expires = _Expires;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetNotes(NStr::CStrSecure const &_Notes) && -> CSecretProperties &&
	{
		m_Notes = _Notes;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_MetadataValue) && -> CSecretProperties &&
	{
		if (!m_Metadata)
			m_Metadata = NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON>{};
		(*m_Metadata)[_MetadataKey] = _MetadataValue;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetCreated(NTime::CTime const &_Created) && -> CSecretProperties &&
	{
		m_Created = _Created;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetModified(NTime::CTime const &_Modified) && -> CSecretProperties &&
	{
		m_Modified = _Modified;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetSemanticID(NStr::CStrSecure const &_SemanticID) && -> CSecretProperties &&
	{
		m_SemanticID = _SemanticID;
		return fg_Move(*this);
	}
	
	auto CSecretsManager::CSecretProperties::f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) && -> CSecretProperties &&
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

	auto CSecretsManager::CSecretProperties::f_SetSecret(CSecret &&_Secret) & -> CSecretProperties &
	{
		m_Secret = fg_Move(_Secret);
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetUserName(NStr::CStrSecure const &_UserName) & -> CSecretProperties &
	{
		m_UserName = _UserName;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetURL(NStr::CStrSecure const &_URL) & -> CSecretProperties &
	{
		m_URL = _URL;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetExpires(NTime::CTime const &_Expires) & -> CSecretProperties &
	{
		m_Expires = _Expires;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetNotes(NStr::CStrSecure const &_Notes) & -> CSecretProperties &
	{
		m_Notes = _Notes;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetMetadata(NStr::CStrSecure const &_MetadataKey, NEncoding::CEJSON &&_MetadataValue) & -> CSecretProperties &
	{
		if (!m_Metadata)
			m_Metadata = NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON>{};
		(*m_Metadata)[_MetadataKey] = _MetadataValue;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetCreated(NTime::CTime const &_Created) & -> CSecretProperties &
	{
		m_Created = _Created;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetModified(NTime::CTime const &_Modified) & -> CSecretProperties &
	{
		m_Modified = _Modified;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetSemanticID(NStr::CStrSecure const &_SemanticID) & -> CSecretProperties &
	{
		m_SemanticID = _SemanticID;
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_SetTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) & -> CSecretProperties &
	{
		m_Tags = fg_Move(_Tags);
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_AddTags(NContainer::TCSet<NStr::CStrSecure> &&_Tags) & -> CSecretProperties &
	{
		if (!m_Tags)
			m_Tags = NContainer::TCSet<NStr::CStrSecure>{};
		*m_Tags += fg_Move(_Tags);
		return *this;
	}

	auto CSecretsManager::CSecretProperties::f_GetSecret() const -> CSecret const &
	{
		static CSecret s_Secret;
		return m_Secret.f_Get(s_Secret);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetUserName() const -> NStr::CStrSecure const &
	{
		static NStr::CStrSecure s_UserName;
		return m_UserName.f_Get(s_UserName);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetURL() const -> NStr::CStrSecure const &
	{
		static NStr::CStrSecure s_URL;
		return m_URL.f_Get(s_URL);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetExpires() const -> NTime::CTime const &
	{
		static NTime::CTime s_Expires;
		return m_Expires.f_Get(s_Expires);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetNotes() const -> NStr::CStrSecure const &
	{
		static NStr::CStrSecure s_Notes;
		return m_Notes.f_Get(s_Notes);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetMetadata() const -> NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON> const &
	{
		static NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON> s_Metadata;
		return m_Metadata.f_Get(s_Metadata);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetCreated() const -> NTime::CTime const &
	{
		static NTime::CTime s_Created;
		return m_Created.f_Get(s_Created);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetModified() const -> NTime::CTime const &
	{
		static NTime::CTime s_Modified;
		return m_Modified.f_Get(s_Modified);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetSemanticID() const -> NStr::CStrSecure const &
	{
		static NStr::CStrSecure s_SemanticID;
		return m_SemanticID.f_Get(s_SemanticID);
	}
	
	auto CSecretsManager::CSecretProperties::f_GetTags() const -> NContainer::TCSet<NStr::CStrSecure> const &
	{
		static NContainer::TCSet<NStr::CStrSecure> s_Tags;
		return m_Tags.f_Get(s_Tags);
	}

	bool CSecretsManager::CSecretProperties::operator == (CSecretsManager::CSecretProperties const &_Right) const
	{
		return
			(
				m_Secret == _Right.m_Secret
				&& m_UserName == _Right.m_UserName
				&& m_URL == _Right.m_URL
				&& m_Expires == _Right.m_Expires
				&& m_Notes == _Right.m_Notes
				&& m_Metadata == _Right.m_Metadata
				&& m_Created == _Right.m_Created
				&& m_Modified == _Right.m_Modified
				&& m_SemanticID == _Right.m_SemanticID
			 )
		;
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
