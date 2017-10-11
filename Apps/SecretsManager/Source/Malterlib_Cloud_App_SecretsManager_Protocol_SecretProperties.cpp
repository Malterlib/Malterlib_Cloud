// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	namespace
	{
		bool fg_HasAllTagsSet(TCOptional<TCSet<CStrSecure>> const &_CurrentTags, TCSet<CStrSecure> const &_NeededTags)
		{
			if (_NeededTags.f_IsEmpty())
				return true;
			
			if (!_CurrentTags)
				return false;
			
			for (auto const &Tag : _NeededTags)
			{
				if (!_CurrentTags->f_FindEqual(Tag))
					return false;
			}
			return true;
		}
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_EnumerateSecrets
		(
			TCOptional<CStrSecure> &_SemanticID
			, TCSet<CStrSecure> const &_TagsExclusive
		) -> TCContinuation<TCSet<CSecretID>>
	{
		TCSet<CSecretID> IDs;
		for (auto const &Secret : m_pThis->m_Secrets)
		{
			if (_SemanticID)
			{
				if (!Secret.m_SemanticID || *_SemanticID != *Secret.m_SemanticID)
					continue;
			}

			if (!fg_HasAllTagsSet(Secret.m_Tags, _TagsExclusive))
				continue;

			IDs[m_pThis->m_Secrets.fs_GetKey(Secret)];
		}
		return fg_Explicit(fg_Move(IDs));
	}

	TCContinuation<CSecretsManager::CSecret> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecret(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;

		auto Auditor = This.mp_AppState.f_Auditor();
		if (auto *pSecret = m_pThis->m_Secrets.f_FindEqual(_ID))
		{
			if (pSecret->m_Secret)
				return fg_Explicit(*pSecret->m_Secret);
			else
				return Auditor.f_Exception("No secret set");
		}
		else
			return Auditor.f_Exception("SecretID does not exist");
	}

	TCContinuation<CSecretsManager::CSecretProperties> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretProperties(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto *pSecret = This.m_Secrets.f_FindEqual(_ID))
			return fg_Explicit(*pSecret);
		else
			return Auditor.f_Exception("SecretID does not exist");
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive)
		-> TCContinuation<CSecretsManager::CSecret>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		CSecretsManager::CSecret FoundSecret;
		int32 nFoundCount = 0;
		
		for (auto const &Secret : This.m_Secrets)
		{
			if (!Secret.m_SemanticID)
				continue;

			if (!fg_HasAllTagsSet(Secret.m_Tags, _TagsExclusive))
				continue;
			
			if (*Secret.m_SemanticID == _SemanticID)
			{
				if (Secret.m_Secret)
				{
					if (++nFoundCount > 1)
						return Auditor.f_Exception("Non-unique Semantic ID");
					FoundSecret = *Secret.m_Secret;
				}
				else
				{
					// Semantic ID matches, but no Secret???
				}
			}
		}

		if (nFoundCount == 0)
			return Auditor.f_Exception("Non matching Semantic ID");
			
		return fg_Explicit(FoundSecret);
	}
	
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetSecretProperties(CSecretsManager::CSecretID &&_ID, CSecretsManager::CSecretProperties &&_Secret)
		-> TCContinuation<void>
	{
		auto &This = *m_pThis;

		This.m_Secrets[_ID] = _Secret;
		
		TCContinuation<void> Continuation;
		Continuation.f_SetResult();
		return Continuation;
	}

	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_ModifyTags
		(
			CSecretsManager::CSecretID &&_ID
			, TCSet<CStrSecure> &&_TagsToRemove
			, TCSet<CStrSecure> &&_TagsToAdd
		)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		
		if (auto *pSecret = This.m_Secrets.f_FindEqual(_ID))
		{
			if (!pSecret->m_Tags)
				pSecret->m_Tags = NContainer::TCSet<NStr::CStrSecure>{};
			*pSecret->m_Tags -=_TagsToRemove;
			*pSecret->m_Tags +=_TagsToAdd;
			return fg_Explicit();
		}
		else
			return Auditor.f_Exception("SecretID does not exist");
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		
		if (auto *pSecret = This.m_Secrets.f_FindEqual(_ID))
		{
			if (!pSecret->m_Metadata)
				pSecret->m_Metadata = NContainer::TCMap<NStr::CStrSecure, NEncoding::CEJSON>{};
			(*pSecret->m_Metadata)[_MetadataKey] = _Metadata;
			return fg_Explicit();
		}
		else
			return Auditor.f_Exception("SecretID does not exist");
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		
		if (auto *pSecret = This.m_Secrets.f_FindEqual(_ID))
		{
			if (pSecret->m_Metadata)
				(*pSecret->m_Metadata).f_Remove(_MetadataKey);
			return fg_Explicit();
		}
		else
			return Auditor.f_Exception("SecretID does not exist");
	}
}
