// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	namespace
	{
		bool fg_HasAllTagsSet(TCSet<CStrSecure> const &_CurrentTags, TCSet<CStrSecure> const &_NeededTags)
		{
			for (auto const &Tag : _NeededTags)
			{
				if (!_CurrentTags.f_FindEqual(Tag))
					return false;
			}
			return true;
		}

		template<class t_CFunctor>
		void fg_CollectMatching
			(
				TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
				, TCOptional<CStrSecure> const &_SemanticID
				, TCSet<CStrSecure> const &_TagsExclusive
				, t_CFunctor _Func
			)
		{
			for (auto const &pSecretProperty : _Secrets)
			{
				if (_SemanticID && *_SemanticID != pSecretProperty.m_SemanticID)
					continue;

				if (!fg_HasAllTagsSet(pSecretProperty.m_Tags, _TagsExclusive))
					continue;

				_Func(&pSecretProperty);
			}
		}

		CStr fg_SecretIDException(CSecretsManager::CSecretID const &_ID)
		{
			return fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name);
		}

		CStr fg_SemanticIDException(CStr const &_Error, CStr const &_SemanticID, TCSet<CStrSecure> const &_Tags)
		{
			CStr TagsString;
			if (!_Tags.f_IsEmpty())
			{
				TagsString += fg_Format(" and Tag{}: ", &"s"[_Tags.f_HasOneMember()]);
				CStr Comma;
				for (auto const &Tag : _Tags)
				{
					TagsString += fg_Format("{}'{}'", Comma, Tag);
					Comma = ", ";
				}
			}
			return fg_Format("{} matching Semantic ID: '{}'{}", _Error, _SemanticID, TagsString);
		}
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_EnumerateSecrets
		(
			TCOptional<CStrSecure> const &_SemanticID
			, TCSet<CStrSecure> const &_TagsExclusive
		) -> TCContinuation<TCSet<CSecretID>>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID(); 

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/EnumerateSecrets"))
			return Auditor.f_AccessDenied("(EnumerateSecrets, command)");

		TCSet<CSecretID> IDs;
		fg_CollectMatching
			(
				m_pThis->mp_Database.m_Secrets
				, _SemanticID
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const *_pSecretProperty)
				{
					// Without permission, do not enumerate this secret
					CStr Permission;
					if (!This.fp_HasPermission("Read", _pSecretProperty->m_SemanticID, _pSecretProperty->m_Tags, Permission))
						return;
					
					IDs[m_pThis->mp_Database.m_Secrets.fs_GetKey(_pSecretProperty)];
				}
			 )
		;
		return fg_Explicit(fg_Move(IDs));
	}

	TCContinuation<CSecretsManager::CSecret> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecret(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/GetSecret"))
			return Auditor.f_AccessDenied("(GetSecret, command)");
		
		if (auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if (!This.fp_HasPermission("Read", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(GetSecret, no permission for '{}')", Permission));
			
			return fg_Explicit(pSecretProperty->m_Secret);
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}

	TCContinuation<CSecretsManager::CSecretProperties> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretProperties(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/GetSecretProperties"))
			return Auditor.f_AccessDenied("(GetSecretProperties, command)");

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if (!This.fp_HasPermission("Read", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(GetSecretProperties, no permission for '{}')", Permission));
			
			return fg_Explicit
				(
				 	CSecretsManager::CSecretProperties
					{
						pSecretProperty->m_Secret
		 				, pSecretProperty->m_UserName
		 				, pSecretProperty->m_URL
		 				, pSecretProperty->m_Expires
		 				, pSecretProperty->m_Notes
		 				, pSecretProperty->m_Metadata
		 				, pSecretProperty->m_Created
		 				, pSecretProperty->m_Modified
		 				, pSecretProperty->m_SemanticID
		 				, pSecretProperty->m_Tags
					}
				)
		   ;
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive)
		-> TCContinuation<CSecretsManager::CSecret>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/GetSecretBySemanticID"))
			return Auditor.f_AccessDenied("(GetSecretBySemanticID, command)");

		CSecretsManager::CSecret FoundSecret;
		int32 nFoundCount = 0;

		fg_CollectMatching
			(
				m_pThis->mp_Database.m_Secrets
				, _SemanticID
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const *_pSecretProperty)
				{
					// Without permission, do not enumerate this secret
					CStr Permission;
					if (!This.fp_HasPermission("Read", _pSecretProperty->m_SemanticID, _pSecretProperty->m_Tags, Permission))
						return;

					++nFoundCount;
					FoundSecret = _pSecretProperty->m_Secret;
				}
			 )
		;

		if (nFoundCount == 0)
			return Auditor.f_Exception(fg_SemanticIDException("No secret", _SemanticID, _TagsExclusive));
			
		if (nFoundCount > 1)
			return Auditor.f_Exception(fg_SemanticIDException("Multiple secrets",_SemanticID, _TagsExclusive));

		return fg_Explicit(FoundSecret);
	}
	
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetSecretProperties(CSecretsManager::CSecretID &&_ID, CSecretsManager::CSecretProperties &&_Secret)
		-> TCContinuation<void>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/SetSecretProperties"))
			return Auditor.f_AccessDenied("(SetSecretProperties, command)");

		// Check the correctness of Semantic ID and Tags *before* we use them to match permissions
		if (_Secret.m_SemanticID)
		{
			if (!CSecretsManager::fs_IsValidTag(*_Secret.m_SemanticID))
				return Auditor.f_Exception(fg_Format("Malformed Semantic ID: '{}'", *_Secret.m_SemanticID));
		}
		if (_Secret.m_Tags)
		{
			for (auto const &Tag : *_Secret.m_Tags)
			{
				if (!CSecretsManager::fs_IsValidTag(Tag))
					return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
			}
		}
		if (_Secret.m_Secret)
		{
			if (_Secret.m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
				return Auditor.f_Exception(fg_Format("The secret cannot be a file secret"));
		}
		auto *pSecretProperty =	This.mp_Database.m_Secrets.f_FindEqual(_ID);
		
		auto CurrentTime = CTime::fs_NowUTC();
		auto *pCreatedOrModified = "modified";
		
		if (pSecretProperty)
		{
			// Must have permission to write to the old semantic ID and tags
			CStr Permission;
			if (!This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(SetSecretProperties, no permission for '{}')", Permission));

			// If SemanticID and/or Tags is changed we must also have permission for the new combo
			if (_Secret.m_SemanticID || _Secret.m_Tags)
			{
				if 
					(
						!This.fp_HasPermission
						(
							 "Write"
							 , _Secret.m_SemanticID ? *_Secret.m_SemanticID : pSecretProperty->m_SemanticID
							 , _Secret.m_Tags ? *_Secret.m_Tags : pSecretProperty->m_Tags
							 , Permission
						)
					)
				{
					return Auditor.f_AccessDenied(fg_Format("(SetSecretProperties, no permission for '{}')", Permission));
				}
			}
		}
		else
		{
			// Must have permission for the SemanticID and Tags we're creating, so use the
			// getter functions to get the empty default values in case they are not set
			CStr Permission;
			if (!This.fp_HasPermission("Write", _Secret.f_GetSemanticID(), _Secret.f_GetTags(), Permission))
				return Auditor.f_AccessDenied(fg_Format("(SetSecretProperties, no permission for '{}')", Permission));

			pSecretProperty = &This.mp_Database.m_Secrets[_ID];
			pSecretProperty->m_Created = CurrentTime;
			pCreatedOrModified = "created";
		}
		
		pSecretProperty->m_Modified = CurrentTime;

		// Only set the members that are defined in _Secret
		CStr RandomFileNameToUnreference;
		if (_Secret.m_Secret)
		{
			if (pSecretProperty->m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
			{
				RandomFileNameToUnreference = pSecretProperty->m_RandomFileName;
				// Keep the key, it is enough to generate a new IV for the next file
				pSecretProperty->m_IV.f_Clear();
				pSecretProperty->m_HMACKey.f_Clear();
				pSecretProperty->m_RandomFileName.f_Clear();
			}
			pSecretProperty->m_Secret = *_Secret.m_Secret;
		}
		if (_Secret.m_UserName)
			pSecretProperty->m_UserName = *_Secret.m_UserName;
			
		if (_Secret.m_URL)
			pSecretProperty->m_URL = *_Secret.m_URL;
			
		if (_Secret.m_Expires)
			pSecretProperty->m_Expires = *_Secret.m_Expires;
			
		if (_Secret.m_Notes)
			pSecretProperty->m_Notes = *_Secret.m_Notes;
			
		if (_Secret.m_Metadata)
			pSecretProperty->m_Metadata = *_Secret.m_Metadata;
			
		if (_Secret.m_Created)
			pSecretProperty->m_Created = *_Secret.m_Created;
		
		if (_Secret.m_Modified)
			pSecretProperty->m_Modified = *_Secret.m_Modified;

		if (_Secret.m_SemanticID)
		{
			This.fp_UpdateSemanticIDs(pSecretProperty->m_SemanticID, *_Secret.m_SemanticID);
			pSecretProperty->m_SemanticID = *_Secret.m_SemanticID;
		}
		
		if (_Secret.m_Tags)
		{
			This.fp_UpdateTags(pSecretProperty->m_Tags, *_Secret.m_Tags);
			pSecretProperty->m_Tags = *_Secret.m_Tags;
		}

		Auditor.f_Info(fg_Format("Secret properties {} for ID '{}/{}'", pCreatedOrModified, _ID.m_Folder, _ID.m_Name));

		This.fp_WriteDatabase();

		if (RandomFileNameToUnreference)
			return This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);
		else
			return fg_Explicit();
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
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/ModifyTags"))
			return Auditor.f_AccessDenied("(ModifyTags, command)");

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			// Check the correctness of Semantic ID and Tags *before* we use them to match permissions
			for (auto const &Tag : _TagsToRemove)
			{
				if (!CSecretsManager::fs_IsValidTag(Tag))
					return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
			}
			for (auto const &Tag : _TagsToAdd)
			{
				if (!CSecretsManager::fs_IsValidTag(Tag))
					return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
			}

			CStr Permission;
			if
				(
					!This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission)
					|| (!_TagsToRemove.f_IsEmpty() && !This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, _TagsToRemove, Permission))
					|| (!_TagsToAdd.f_IsEmpty() && !This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, _TagsToAdd, Permission))
				)
			{
				return Auditor.f_AccessDenied(fg_Format("(ModifyTags, no permission for '{}')", Permission));
			}
			
			// If we remove everything from the tags set we must have NoTags permission
			auto CurrentTags(pSecretProperty->m_Tags);
			CurrentTags -= _TagsToRemove;
			if (CurrentTags.f_IsEmpty() && !This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, CurrentTags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(ModifyTags, no permission for '{}')", Permission));

			This.fp_UpdateTags(_TagsToRemove, _TagsToAdd);

			pSecretProperty->m_Tags -= _TagsToRemove;
			pSecretProperty->m_Tags += _TagsToAdd;
			pSecretProperty->m_Modified = CTime::fs_NowUTC();
			
			Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));
			
			This.fp_WriteDatabase();

			return fg_Explicit();
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/SetMetadata"))
			return Auditor.f_AccessDenied("(SetMetadata, command)");
		
		if (auto *pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if	(!This.fp_HasPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(SetMetadata, no permission for '{}')", Permission));

			pSecretProperties->m_Metadata[_MetadataKey] = _Metadata;
			pSecretProperties->m_Modified = CTime::fs_NowUTC();
			
			Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));
			
			This.fp_WriteDatabase();

			return fg_Explicit();
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/RemoveMetadata"))
			return Auditor.f_AccessDenied("(RemoveMetadata, command)");

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if	(!This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(RemoveMetadata, no permission for '{}')", Permission));

			pSecretProperty->m_Metadata.f_Remove(_MetadataKey);
			pSecretProperty->m_Modified = CTime::fs_NowUTC();

			Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

			This.fp_WriteDatabase();

			return fg_Explicit();
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}

	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveSecret(CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/RemoveSecret"))
			return Auditor.f_AccessDenied("(RemoveSecret, command)");

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if	(!This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(RemoveSecret, no permission for '{}')", Permission));

			CStr RandomFileNameToUnreference;
			if (pSecretProperty->m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
				RandomFileNameToUnreference = pSecretProperty->m_RandomFileName;

			This.mp_Database.m_Secrets.f_Remove(_ID);

			Auditor.f_Info(fg_Format("Secret removed, ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

			This.fp_WriteDatabase();

			if (RandomFileNameToUnreference)
				return This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);
			else
				return fg_Explicit();
		}
		else
			return Auditor.f_Exception(fg_SecretIDException(_ID));
	}
}
