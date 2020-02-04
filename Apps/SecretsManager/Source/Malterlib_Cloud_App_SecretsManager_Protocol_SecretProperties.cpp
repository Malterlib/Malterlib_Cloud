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
		void fg_CollectMatchingSecrets
			(
				TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
				, TCOptional<CStrSecure> const &_SemanticID
				, TCSet<CStrSecure> const &_TagsExclusive
				, t_CFunctor _Func
			)
		{
			for (auto const &SecretProperty : _Secrets)
			{
				if (_SemanticID && fg_StrMatchWildcard(SecretProperty.m_SemanticID.f_GetStr(), _SemanticID->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
					continue;

				if (!fg_HasAllTagsSet(SecretProperty.m_Tags, _TagsExclusive))
					continue;

				_Func(SecretProperty);
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

		CExceptionPointer fg_ValidateID(CSecretsManager::CSecretID const &_ID, CDistributedAppAuditor const &_Auditor)
		{
			if (!CSecretsManager::fs_IsValidFolder(_ID.m_Folder))
				return _Auditor.f_Exception(fg_Format("Malformed folder: '{}'", _ID.m_Folder)).f_ExceptionPointer();

			if (!CSecretsManager::fs_IsValidName(_ID.m_Name))
				return _Auditor.f_Exception(fg_Format("Malformed name: '{}'", _ID.m_Name)).f_ExceptionPointer();

			return nullptr;
		}
	}

	void CSecretsManagerDaemonActor::CServer::fsp_AddPermissionsForMatchingSecrets
		(
			TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
			, TCOptional<CStrSecure> const &_SemanticID
			, TCSet<CStrSecure> const &_TagsExclusive
			, NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> &o_Permissions
		)
	{
		fg_CollectMatchingSecrets
			(
				_Secrets
				, _SemanticID
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperty)
				{
					fsp_AddPermissionQueryIndexedBySecretID
						(
							_Secrets.fs_GetKey(_SecretProperty)
							, "Read"
							, _SecretProperty.m_SemanticID
							, _SecretProperty.m_Tags
							, o_Permissions
						)
					;
				}
			 )
		;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_EnumerateSecrets
		(
			TCOptional<CStrSecure> const &_SemanticID
			, TCSet<CStrSecure> const &_TagsExclusive
		) -> TCFuture<TCSet<CSecretID>>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (_SemanticID && !CSecretsManager::fs_IsValidSemanticIDWildcard(*_SemanticID))
			co_return Auditor.f_Exception(fg_Format("Malformed semantic ID: '{}'", *_SemanticID));

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/EnumerateSecrets"}};
		fsp_AddPermissionsForMatchingSecrets(m_pThis->mp_Database.m_Secrets, _SemanticID, _TagsExclusive, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Enumerate secret in SecretsManager", Permissions) % "Permission denied enumerating secrets" % Auditor);
		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(EnumerateSecrets, command)");

		TCSet<CSecretID> IDs;
		fg_CollectMatchingSecrets
			(
				m_pThis->mp_Database.m_Secrets
				, _SemanticID
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperty)
				{
					// Only enumerate secrets the host is allowed to see
					auto const &ID = m_pThis->mp_Database.m_Secrets.fs_GetKey(_SecretProperty);
					auto *pHasPermission = HasPermissions.f_FindEqual(CStr::fs_ToStr(ID));
					if (pHasPermission && *pHasPermission)
						IDs[ID];
				}
			)
		;

		co_return fg_Move(IDs);
	}

	TCFuture<CSecretsManager::CSecret> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecret(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecret"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
			fsp_AddPermissionQueryIndexedByPermission("Read", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Get secret from SecretsManager", Permissions) % "Permission denied getting secret" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecret, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(GetSecret, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		co_return pSecretProperty->m_Secret;
	}

	TCFuture<CSecretsManager::CSecretProperties> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretProperties(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecretProperties"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
			fsp_AddPermissionQueryIndexedByPermission("Read", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

		auto HasPermissions = co_await
			(
			 	This.mp_Permissions.f_HasPermissions("Get secret properties from SecretsManager", Permissions)
			 	% "Permission denied getting secret properties"
			 	% Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecretProperties, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(GetSecretProperties, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		co_return CSecretsManager::CSecretProperties
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
		;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive)
		-> TCFuture<CSecretsManager::CSecret>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (!CSecretsManager::fs_IsValidSemanticID(_SemanticID))
			co_return Auditor.f_Exception(fg_Format("Malformed semantic ID: '{}'", _SemanticID));

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecretBySemanticID"}};
		fsp_AddPermissionsForMatchingSecrets(m_pThis->mp_Database.m_Secrets, _SemanticID, _TagsExclusive, Permissions);

		auto HasPermissions = co_await
			(
			 	This.mp_Permissions.f_HasPermissions("Get secret by semantic ID from SecretsManager", Permissions)
			 	% "Permission denied getting secret by semantic ID"
			 	% Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecretBySemanticID, command)");

		CSecretsManager::CSecret FoundSecret;
		int32 nFoundCount = 0;
		fg_CollectMatchingSecrets
			(
				m_pThis->mp_Database.m_Secrets
				, _SemanticID
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperty)
				{
					// Only count secrets host is allowed to see
					auto const &ID = m_pThis->mp_Database.m_Secrets.fs_GetKey(_SecretProperty);
					auto *pHasPermission = HasPermissions.f_FindEqual(CStr::fs_ToStr(ID));
					if (pHasPermission && *pHasPermission)
					{
						++nFoundCount;
						FoundSecret = _SecretProperty.m_Secret;
					}
				}
			)
		;

		if (nFoundCount == 0)
			co_return Auditor.f_Exception(fg_SemanticIDException("No secret", _SemanticID, _TagsExclusive));

		if (nFoundCount > 1)
			co_return Auditor.f_Exception(fg_SemanticIDException("Multiple secrets", _SemanticID, _TagsExclusive));

		co_return fg_Move(FoundSecret);
	}
	
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetSecretProperties(CSecretsManager::CSecretID &&_ID, CSecretsManager::CSecretProperties &&_Secret)
		-> TCFuture<void>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		// Check the correctness of Semantic ID and Tags before we use them to match permissions
		if (_Secret.m_SemanticID)
		{
			if (!CSecretsManager::fs_IsValidSemanticID(*_Secret.m_SemanticID))
				co_return Auditor.f_Exception(fg_Format("Malformed Semantic ID: '{}'", *_Secret.m_SemanticID));
		}
		if (_Secret.m_Tags)
		{
			for (auto const &Tag : *_Secret.m_Tags)
			{
				if (!CSecretsManager::fs_IsValidTag(Tag))
					co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
			}
		}
		if (_Secret.m_Secret)
		{
			if (_Secret.m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
				co_return Auditor.f_Exception(fg_Format("The secret cannot be a file secret"));
		}

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/SetSecretProperties"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
		{
			// Must have permission to write to the old secret
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);
			// As well as the the new seamtic ID/tags combo
			if (_Secret.m_SemanticID || _Secret.m_Tags)
			{
				fsp_AddPermissionQueryIndexedByPermission
					(
					 	"Write"
						, _Secret.m_SemanticID ? *_Secret.m_SemanticID : pSecretProperty->m_SemanticID
						, _Secret.m_Tags ? *_Secret.m_Tags : pSecretProperty->m_Tags
						, Permissions
					)
				;
			}
		}
		else
			fsp_AddPermissionQueryIndexedByPermission("Write", _Secret.f_GetSemanticID(), _Secret.f_GetTags(), Permissions);

		auto HasPermissions = co_await
			(
			 	This.mp_Permissions.f_HasPermissions("Set secret properties in SecretsManager", Permissions)
			 	% "Permission denied setting secret properties"
			 	% Auditor
			)
		;
		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(SetSecretProperties, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(SetSecretProperties, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		auto CurrentTime = CTime::fs_NowUTC();
		auto *pCreatedOrModified = "modified";

		if (!pSecretProperty)
		{
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

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		if (RandomFileNameToUnreference)
			co_await This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_ModifyTags
		(
			CSecretsManager::CSecretID &&_ID
			, TCSet<CStrSecure> &&_TagsToRemove
			, TCSet<CStrSecure> &&_TagsToAdd
		)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		// Check the correctness of Semantic ID and Tags before we use them to match permissions
		for (auto const &Tag : _TagsToRemove)
		{
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
		}

		for (auto const &Tag : _TagsToAdd)
		{
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
		}

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/ModifyTags"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
		{
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

			if (!_TagsToRemove.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, _TagsToRemove, Permissions);

			if (!_TagsToAdd.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, _TagsToAdd, Permissions);

			// If we remove everything from the tags set we must have NoTags permission
			auto CurrentTags(pSecretProperty->m_Tags);
			CurrentTags -= _TagsToRemove;
			if (CurrentTags.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, CurrentTags, Permissions);
		}

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Modify tags in SecretsManager", Permissions) % "Permission denied modifying tags" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(ModifyTags, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(ModifyTags, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		This.fp_UpdateTags(_TagsToRemove, _TagsToAdd);

		pSecretProperty->m_Tags -= _TagsToRemove;
		pSecretProperty->m_Tags += _TagsToAdd;
		pSecretProperty->m_Modified = CTime::fs_NowUTC();

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/SetMetadata"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Set metadata in SecretsManager", Permissions) % "Permission denied setting meta data" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(SetMetadata, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(SetMetadata, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		pSecretProperty->m_Metadata[_MetadataKey] = _Metadata;
		pSecretProperty->m_Modified = CTime::fs_NowUTC();

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/RemoveMetadata"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Remove metadata in SecretsManager", Permissions) % "Permission denied removing meta data" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(RemoveMetadata, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(RemoveMetadata, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		pSecretProperty->m_Metadata.f_Remove(_MetadataKey);
		pSecretProperty->m_Modified = CTime::fs_NowUTC();

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveSecret(CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/RemoveSecret"}};
		auto *pSecretProperty = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperty)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Remove secret in SecretsManager", Permissions) % "Permission denied removing secret" % Auditor);
		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(RemoveSecret, command)");

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
				co_return Auditor.f_AccessDenied(fg_Format("(RemoveSecret, no permission for '{}')", HasPermissions.fs_GetKey(bHasPermission)));
		}

		if (!pSecretProperty)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		CStr RandomFileNameToUnreference;
		if (pSecretProperty->m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
			RandomFileNameToUnreference = pSecretProperty->m_RandomFileName;

		This.mp_Database.m_Secrets.f_Remove(_ID);

		Auditor.f_Info(fg_Format("Secret removed, ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		if (RandomFileNameToUnreference)
			co_await This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);

		co_return {};
	}
}
