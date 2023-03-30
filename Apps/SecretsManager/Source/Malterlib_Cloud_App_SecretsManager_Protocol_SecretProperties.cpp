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
				, TCOptional<CStrSecure> const &_Name
				, TCSet<CStrSecure> const &_TagsExclusive
				, t_CFunctor _Func
			)
		{
			for (auto const &SecretProperties : _Secrets)
			{
				if (CSecretsManagerDaemonActor::CServer::fs_MatchSecret(SecretProperties, _SemanticID, _Name, _TagsExclusive))
					_Func(SecretProperties);
			}
		}

		CStr fg_SecretIDException(CSecretsManager::CSecretID const &_ID)
		{
			return fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name);
		}

		CStr fg_SemanticIDException(CStr const &_Error, CStr const &_SemanticID, NStorage::TCOptional<CStrSecure> const &_Name, TCSet<CStrSecure> const &_Tags)
		{
			CStr TagsString;
			if (!_Tags.f_IsEmpty())
			{
				TagsString += fg_Format(" and Tag{}: ", &"s"[_Tags.f_HasOneElement()]);
				CStr Comma;
				for (auto const &Tag : _Tags)
				{
					TagsString += fg_Format("{}'{}'", Comma, Tag);
					Comma = ", ";
				}
			}

			CStr NameString;
			if (_Name)
				NameString = fg_Format(" and Name: {}", *_Name);

			return fg_Format("{} matching Semantic ID: '{}'{}{}", _Error, _SemanticID, NameString, TagsString);
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

	bool CSecretsManagerDaemonActor::CServer::fs_MatchSecret
		(
			CSecretPropertiesInternal const &_SecretProperties
			, TCOptional<CStrSecure> const &_SemanticID
			, TCOptional<CStrSecure> const &_Name
			, TCSet<CStrSecure> const &_TagsExclusive
		)
	{
		if (_SemanticID && fg_StrMatchWildcard(_SecretProperties.m_SemanticID.f_GetStr(), _SemanticID->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
			return false;

		if (_Name && fg_StrMatchWildcard(_SecretProperties.f_GetSecretID().m_Name.f_GetStr(), _Name->f_GetStr()) != EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
			return false;

		if (!fg_HasAllTagsSet(_SecretProperties.m_Tags, _TagsExclusive))
			return false;

		return true;
	}

	void CSecretsManagerDaemonActor::CServer::fsp_AddPermissionsForMatchingSecrets
		(
			TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
			, TCOptional<CStrSecure> const &_SemanticID
			, TCOptional<CStrSecure> const &_Name
			, TCSet<CStrSecure> const &_TagsExclusive
			, NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> &o_Permissions
		)
	{
		fg_CollectMatchingSecrets
			(
				_Secrets
				, _SemanticID
				, _Name
				, _TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperties)
				{
					fsp_AddPermissionQueryIndexedBySecretID
						(
							_Secrets.fs_GetKey(_SecretProperties)
							, "Read"
							, _SecretProperties.m_SemanticID
							, _SecretProperties.m_Tags
							, o_Permissions
						)
					;
				}
			 )
		;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_EnumerateSecrets(CEnumerateSecrets const &_Options) -> TCFuture<TCSet<CSecretID>>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (_Options.m_SemanticID && !CSecretsManager::fs_IsValidSemanticIDWildcard(*_Options.m_SemanticID))
			co_return Auditor.f_Exception(fg_Format("Malformed semantic ID: '{}'", *_Options.m_SemanticID));

		if (_Options.m_Name && !CSecretsManager::fs_IsValidNameWildcard(*_Options.m_Name))
			co_return Auditor.f_Exception(fg_Format("Malformed name: '{}'", *_Options.m_Name));

		for (auto const &Tag : _Options.m_TagsExclusive)
		{
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
		}

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/EnumerateSecrets"}};
		fsp_AddPermissionsForMatchingSecrets(m_pThis->mp_Database.m_Secrets, _Options.m_SemanticID, _Options.m_Name, _Options.m_TagsExclusive, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Enumerate secret in SecretsManager", Permissions) % "Permission denied enumerating secrets" % Auditor);
		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(EnumerateSecrets, command)", Permissions["Command"]);

		TCSet<CSecretID> IDs;
		fg_CollectMatchingSecrets
			(
				m_pThis->mp_Database.m_Secrets
				, _Options.m_SemanticID
				, _Options.m_Name
				, _Options.m_TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperties)
				{
					// Only enumerate secrets the host is allowed to see
					auto const &ID = m_pThis->mp_Database.m_Secrets.fs_GetKey(_SecretProperties);
					auto *pHasPermission = HasPermissions.f_FindEqual(CStr::fs_ToStr(ID));
					if (pHasPermission && *pHasPermission)
						IDs[ID];
				}
			)
		;

		Auditor.f_Info(fg_Format("Enumerate secrets {vs}", IDs));

		co_return fg_Move(IDs);
	}

	TCFuture<CSecretsManager::CSecret> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecret(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecret"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Read", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Get secret from SecretsManager", Permissions) % "Permission denied getting secret" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecret, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(GetSecret, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		Auditor.f_Info(fg_Format("Read secret {}", _ID));

		co_return pSecretProperties->m_Secret;
	}

	CSecretsManager::CSecretProperties CSecretPropertiesInternal::f_ToSecretProperties() const
	{
		return CSecretsManager::CSecretProperties
			{
				m_Secret
				, m_UserName
				, m_URL
				, m_Expires
				, m_Notes
				, m_Metadata
				, m_Created
				, m_Modified
				, m_SemanticID
				, m_Tags
				, m_bImmutable
			}
		;
	}

	TCFuture<CSecretsManager::CSecretProperties> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretProperties(CSecretsManager::CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecretProperties"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Read", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await
			(
				This.mp_Permissions.f_HasPermissions("Get secret properties from SecretsManager", Permissions)
				% "Permission denied getting secret properties"
				% Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecretProperties, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(GetSecretProperties, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		Auditor.f_Info(fg_Format("Read secret properties {}", _ID));

		co_return pSecretProperties->f_ToSecretProperties();
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_GetSecretBySemanticID(CGetSecretBySemanticID const &_Options)
		-> TCFuture<CSecretsManager::CSecret>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (!CSecretsManager::fs_IsValidSemanticID(_Options.m_SemanticID))
			co_return Auditor.f_Exception(fg_Format("Malformed semantic ID: '{}'", _Options.m_SemanticID));

		for (auto const &Tag : _Options.m_TagsExclusive)
		{
			if (!CSecretsManager::fs_IsValidTag(Tag))
				co_return Auditor.f_Exception(fg_Format("Malformed Tag: '{}'", Tag));
		}

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/GetSecretBySemanticID"}};
		fsp_AddPermissionsForMatchingSecrets(m_pThis->mp_Database.m_Secrets, _Options.m_SemanticID, _Options.m_Name, _Options.m_TagsExclusive, Permissions);

		auto HasPermissions = co_await
			(
				This.mp_Permissions.f_HasPermissions("Get secret by semantic ID from SecretsManager", Permissions)
				% "Permission denied getting secret by semantic ID"
				% Auditor
			)
		;

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(GetSecretBySemanticID, command)", Permissions["Command"]);

		CSecretsManager::CSecret FoundSecret;
		int32 nFoundCount = 0;
		CSecretsManager::CSecretID FoundSecretID;
		fg_CollectMatchingSecrets
			(
				m_pThis->mp_Database.m_Secrets
				, _Options.m_SemanticID
				, _Options.m_Name
				, _Options.m_TagsExclusive
				, [&] (CSecretPropertiesInternal const &_SecretProperties)
				{
					// Only count secrets host is allowed to see
					auto const &ID = m_pThis->mp_Database.m_Secrets.fs_GetKey(_SecretProperties);
					auto *pHasPermission = HasPermissions.f_FindEqual(CStr::fs_ToStr(ID));
					if (pHasPermission && *pHasPermission)
					{
						++nFoundCount;
						FoundSecret = _SecretProperties.m_Secret;
						FoundSecretID = _SecretProperties.f_GetSecretID();
					}
				}
			)
		;

		if (nFoundCount == 0)
			co_return Auditor.f_Exception(fg_SemanticIDException("No secret", _Options.m_SemanticID, _Options.m_Name, _Options.m_TagsExclusive));

		if (nFoundCount > 1)
			co_return Auditor.f_Exception(fg_SemanticIDException("Multiple secrets", _Options.m_SemanticID, _Options.m_Name, _Options.m_TagsExclusive));

		Auditor.f_Info(fg_Format("Read secret by semantic ID {}", FoundSecretID));

		co_return fg_Move(FoundSecret);
	}
	
	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetSecretProperties(CSecretsManager::CSecretID &&_ID, CSecretsManager::CSecretProperties &&_Secret)
		-> TCFuture<CSetSecretPropertiesResult>
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

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/SetSecretProperties"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
		{
			// Must have permission to write to the old secret
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);
			// As well as the the new seamtic ID/tags combo
			if (_Secret.m_SemanticID || _Secret.m_Tags)
			{
				fsp_AddPermissionQueryIndexedByPermission
					(
						"Write"
						, _Secret.m_SemanticID ? *_Secret.m_SemanticID : pSecretProperties->m_SemanticID
						, _Secret.m_Tags ? *_Secret.m_Tags : pSecretProperties->m_Tags
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
			co_return Auditor.f_AccessDenied("(SetSecretProperties, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(SetSecretProperties, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		auto CurrentTime = CTime::fs_NowUTC();
		auto *pCreatedOrModified = "modified";

		bool bOldImmutable = false;
		bool bChanged = false;

		auto fSetCheckChanged = [&](auto &o_Destination, auto const &_Value)
			{
				if (_Value != o_Destination)
				{
					o_Destination = _Value;
					bChanged = true;
				}
			}
		;

		CSetSecretPropertiesResult Result{ESetSecretPropertiesResultFlag_None};

		if (!pSecretProperties)
		{
			pSecretProperties = &This.mp_Database.m_Secrets[_ID];
			pSecretProperties->m_Created = CurrentTime;
			pCreatedOrModified = "created";
			bChanged = true;
			Result.m_Flags |= ESetSecretPropertiesResultFlag_Created;
		}
		else
			bOldImmutable = pSecretProperties->m_bImmutable;

		if (_Secret.m_Immutable)
		{
			if (bOldImmutable)
			{
				if (!*(_Secret.m_Immutable))
					co_return Auditor.f_Exception("Immutable cannot be removed from an immutable secret");
			}

			fSetCheckChanged(pSecretProperties->m_bImmutable, *_Secret.m_Immutable);
		}

		// Only set the members that are defined in _Secret
		CStr RandomFileNameToUnreference;
		if (_Secret.m_Secret)
		{
			if (bOldImmutable && *_Secret.m_Secret != pSecretProperties->m_Secret)
				co_return Auditor.f_Exception("Cannot change secret on an immutable secret");

			if (pSecretProperties->m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
			{
				RandomFileNameToUnreference = pSecretProperties->m_RandomFileName;
				// Keep the key, it is enough to generate a new IV for the next file
				pSecretProperties->m_IV.f_Clear();
				pSecretProperties->m_HMACKey.f_Clear();
				pSecretProperties->m_RandomFileName.f_Clear();
			}
			fSetCheckChanged(pSecretProperties->m_Secret, *_Secret.m_Secret);
		}

		if (_Secret.m_UserName)
			fSetCheckChanged(pSecretProperties->m_UserName, *_Secret.m_UserName);

		if (_Secret.m_URL)
			fSetCheckChanged(pSecretProperties->m_URL, *_Secret.m_URL);

		if (_Secret.m_Expires)
			fSetCheckChanged(pSecretProperties->m_Expires, *_Secret.m_Expires);

		if (_Secret.m_Notes)
			fSetCheckChanged(pSecretProperties->m_Notes, *_Secret.m_Notes);

		if (_Secret.m_Metadata)
			fSetCheckChanged(pSecretProperties->m_Metadata, *_Secret.m_Metadata);

		if (_Secret.m_Created)
			fSetCheckChanged(pSecretProperties->m_Created, *_Secret.m_Created);

		if (_Secret.m_Modified)
			fSetCheckChanged(pSecretProperties->m_Modified, *_Secret.m_Modified);
		else
			fSetCheckChanged(pSecretProperties->m_Modified, CurrentTime);

		if (_Secret.m_SemanticID)
		{
			if (*_Secret.m_SemanticID != pSecretProperties->m_SemanticID)
			{
				This.fp_UpdateSemanticIDs(pSecretProperties->m_SemanticID, *_Secret.m_SemanticID);
				pSecretProperties->m_SemanticID = *_Secret.m_SemanticID;
				bChanged = true;
			}
		}

		if (_Secret.m_Tags)
		{
			if (*_Secret.m_Tags != pSecretProperties->m_Tags)
			{
				This.fp_UpdateTags(pSecretProperties->m_Tags, *_Secret.m_Tags);
				pSecretProperties->m_Tags = *_Secret.m_Tags;
				bChanged = true;
			}
		}

		Auditor.f_Info(fg_Format("Secret properties {} for ID '{}/{}'", pCreatedOrModified, _ID.m_Folder, _ID.m_Name));

		if (bChanged)
		{
			This.fp_SecretUpdated(*pSecretProperties, false);

			co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

			Result.m_Flags |= ESetSecretPropertiesResultFlag_Updated;
		}

		if (RandomFileNameToUnreference)
			co_await This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);

		co_return fg_Move(Result);
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

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/ModifyTags"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
		{
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

			if (!_TagsToRemove.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, _TagsToRemove, Permissions);

			if (!_TagsToAdd.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, _TagsToAdd, Permissions);

			// If we remove everything from the tags set we must have NoTags permission
			auto CurrentTags(pSecretProperties->m_Tags);
			CurrentTags -= _TagsToRemove;
			if (CurrentTags.f_IsEmpty())
				fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, CurrentTags, Permissions);
		}

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Modify tags in SecretsManager", Permissions) % "Permission denied modifying tags" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(ModifyTags, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(ModifyTags, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		This.fp_UpdateTags(_TagsToRemove, _TagsToAdd);

		pSecretProperties->m_Tags -= _TagsToRemove;
		pSecretProperties->m_Tags += _TagsToAdd;
		pSecretProperties->m_Modified = CTime::fs_NowUTC();

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

		This.fp_SecretUpdated(*pSecretProperties, false);

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_SetMetadata(CSetMetadata &&_SetMetadata)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		auto &ID = _SetMetadata.m_ID;

		if (auto pError = fg_ValidateID(ID, Auditor))
			co_return fg_Move(pError);

		CSecretPropertiesInternal *pSecretProperties;
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(ID);
					return {};
				}
			)
		;

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/SetMetadata"}};

		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Set metadata in SecretsManager", Permissions) % "Permission denied setting meta data" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(SetMetadata, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(SetMetadata, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(ID));

		if (_SetMetadata.m_ExpectedValue)
		{
			auto *pPreviousValue = pSecretProperties->m_Metadata.f_FindEqual(_SetMetadata.m_Key);
			if (!pPreviousValue || *pPreviousValue != *_SetMetadata.m_ExpectedValue)
			{
				Auditor.f_Info("Previous value does not match expected value for '{}'"_f << _SetMetadata.m_Key);
				co_return DMibErrorInstanceSecretsManagerUnexpectedValue("Previous value does not match expected value");
			}
		}

		CTime ModifiedTime;
		if (_SetMetadata.m_ModifiedTime)
			ModifiedTime = *_SetMetadata.m_ModifiedTime;
		else
			ModifiedTime = CTime::fs_NowUTC();

		pSecretProperties->m_Metadata[_SetMetadata.m_Key] = _SetMetadata.m_Value;
		pSecretProperties->m_Modified = ModifiedTime;

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", ID.m_Folder, ID.m_Name));

		This.fp_SecretUpdated(*pSecretProperties, false);

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/RemoveMetadata"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Remove metadata in SecretsManager", Permissions) % "Permission denied removing meta data" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(RemoveMetadata, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(RemoveMetadata, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		pSecretProperties->m_Metadata.f_Remove(_MetadataKey);
		pSecretProperties->m_Modified = CTime::fs_NowUTC();

		Auditor.f_Info(fg_Format("Secret properties modified for ID '{}/{}'", _ID.m_Folder, _ID.m_Name));

		This.fp_SecretUpdated(*pSecretProperties, false);

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		co_return {};
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_RemoveSecret(CSecretID &&_ID)
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (auto pError = fg_ValidateID(_ID, Auditor))
			co_return fg_Move(pError);

		NContainer::TCMap<CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/RemoveSecret"}};
		auto *pSecretProperties = m_pThis->mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Remove secret in SecretsManager", Permissions) % "Permission denied removing secret" % Auditor);
		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(RemoveSecret, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(RemoveSecret, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_SecretIDException(_ID));

		CStr RandomFileNameToUnreference;
		if (pSecretProperties->m_Secret.f_GetTypeID() == CSecretsManager::ESecretType_File)
			RandomFileNameToUnreference = pSecretProperties->m_RandomFileName;

		This.fp_SecretUpdated(*pSecretProperties, true);
		This.mp_Database.m_Secrets.f_Remove(_ID);

		Auditor.f_Info(fg_Format("Secret removed, ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

		co_await (This.fp_WriteDatabase() % "Failed to save database" % Auditor);

		if (RandomFileNameToUnreference)
			co_await This.fp_RemoveUnreferencedFile(RandomFileNameToUnreference, Auditor);

		co_return {};
	}
}
