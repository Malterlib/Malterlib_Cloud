// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState, TCActor<CSecretsManagerServerDatabase> const &_DatabaseActor)
		: mp_AppState(_AppState)
		, mp_DatabaseActor(_DatabaseActor)
		, mp_pCanDestroyTracker(fg_Construct())
	{
		fp_Init();
	}
	
	CSecretsManagerDaemonActor::CServer::~CServer()
	{
	}
	
	void CSecretsManagerDaemonActor::CServer::fp_Init()
	{
		mp_DatabaseActor(&CSecretsManagerServerDatabase::f_ReadDatabase) > [this](TCAsyncResult<CSecretsManagerServerDatabase::CDatabase> &&_Database)
			{
				if (!_Database)
				{
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to read database: {}", _Database.f_GetExceptionStr());
					return;
				}
				
				mp_Database = fg_Move(*_Database);
				for (auto const &SecretProperties : mp_Database)
				{
					fp_UpdateTags({}, SecretProperties.m_Tags);
					fp_UpdateSemanticIDs("", SecretProperties.m_SemanticID);
				}
				
				fp_SetupPermissions() > [this](TCAsyncResult<void> &&_ResultPermissions)
					{
						if (!_ResultPermissions)
						{
							DLogWithCategory(Malterlib/Cloud/SecretsManager, Error, "Failed to setup permissions, aborting startup: {}", _ResultPermissions.f_GetExceptionStr());
							return;
						}
						fp_Publish();
					}
				;
			}
		;
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCContinuation<void> Continuation;
		
		TCSet<CStr> Permissions;
		
		Permissions["SecretsManager/CommandAll"];

		Permissions["SecretsManager/Command/EnumerateSecrets"];
		Permissions["SecretsManager/Command/GetSecret"];
		Permissions["SecretsManager/Command/GetSecretBySemanticID"];
		Permissions["SecretsManager/Command/GetSecretProperties"];
		Permissions["SecretsManager/Command/SetSecretProperties"];
		Permissions["SecretsManager/Command/ModifyTags"];
		Permissions["SecretsManager/Command/SetMetadata"];
		Permissions["SecretsManager/Command/RemoveMetadata"];
		Permissions["SecretsManager/Command/DownloadFile"];

		Permissions["SecretsManager/Read/*/NoTag"];
		Permissions["SecretsManager/Write/*/NoTag"];
		for (auto const &pTag : mp_Tags)
		{
			Permissions[fg_Format("SecretsManager/Read/*/Tag/{}", mp_Tags.fs_GetKey(pTag))];
			Permissions[fg_Format("SecretsManager/Write/*/Tag/{}", mp_Tags.fs_GetKey(pTag))];
		}
		Permissions["SecretsManager/Read/NoSemanticID/*"];
		Permissions["SecretsManager/Write/NoSemanticID/*"];
		for (auto const &pSemanticID : mp_SemanticIDs)
		{
			Permissions[fg_Format("SecretsManager/Read/SemanticID/{}/*", mp_SemanticIDs.fs_GetKey(pSemanticID))];
			Permissions[fg_Format("SecretsManager/Write/SemanticID/{}/*", mp_SemanticIDs.fs_GetKey(pSemanticID))];
		}
		
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
		
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("SecretsManager/*");

		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this))
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		return pCanDestroy->m_Continuation;
	}
	
	bool CSecretsManagerDaemonActor::CServer::fp_HasPermission(char const *_ReadWrite, CStr const &_SemanticID, TCSet<CStrSecure> const &_Tags, CStr &o_Permission)
	{
		auto CallingHostID = fg_GetCallingHostID();
		CStr SemanticPart{ _SemanticID ? fg_Format("SemanticID/{}", _SemanticID) : "NoSemanticID"};

		if (_Tags.f_IsEmpty())
		{
			o_Permission = fg_Format("SecretsManager/{}/{}/NoTag", _ReadWrite, SemanticPart);
			return mp_Permissions.f_HostHasWildcardPermission(CallingHostID, o_Permission);
		}
		else
		{
			for (auto const &Tag : _Tags)
			{
				o_Permission = fg_Format("SecretsManager/{}/{}/Tag/{}", _ReadWrite, SemanticPart, Tag);
				if (!mp_Permissions.f_HostHasWildcardPermission(CallingHostID, o_Permission))
					return false;
			}
		}
		return true;
	}
	
	namespace
	{
		template <class _InputIterator>
		void
		fg_SetIntersection(_InputIterator __first1, _InputIterator __first2,  TCSet<CStr> &_Result)
		{
			while (__first1 && __first2)
			{
				if (*__first1 < *__first2)
					++__first1;
				else
				{
					if (*__first2 == *__first1)
					{
						_Result[*__first1];
						++__first1;
					}
					++__first2;
				}
			}
		}
	}
	
	void CSecretsManagerDaemonActor::CServer::fp_UpdateTags(TCSet<CStrSecure> const &_TagsToRemove,TCSet<CStrSecure> const &_TagsToAdd)
	{
		// Filter out tags in both sets to avoid unnecessary permission registrations
		TCSet<CStr> InBoth;
		fg_SetIntersection(_TagsToRemove.f_GetIterator(), _TagsToAdd.f_GetIterator(), InBoth);
		
		for (auto const &Tag : InBoth)
		{
			mp_Tags.f_Remove(Tag);
			mp_Tags.f_Remove(Tag);
		}

		for (auto const &Tag : _TagsToRemove)
		{
			// Take care: Since we allow "removal" of tags not on the property we should only decrease the count if the count is in the set.
			// Otherwise we risk getting negativ counts and ending up with a too low count when the tags is added later
			if (auto *pCount = mp_Tags.f_FindEqual(Tag))
			{
				if (--*pCount == 0)
				{
					mp_Tags.f_Remove(Tag);
					mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, TCSet<CStr>{fg_Format("SecretsManager/Read/*/Tag/{}", Tag)}) > fg_DiscardResult();
					mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, TCSet<CStr>{fg_Format("SecretsManager/Write/*/Tag/{}", Tag)}) > fg_DiscardResult();
				}
			}
		}

		for (auto const &Tag : _TagsToAdd)
		{
			if (++mp_Tags[Tag] == 1)
			{
				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, TCSet<CStr>{fg_Format("SecretsManager/Read/*/Tag/{}", Tag)}) > fg_DiscardResult();
				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, TCSet<CStr>{fg_Format("SecretsManager/Write/*/Tag/{}", Tag)}) > fg_DiscardResult();
			}
		}
	}

	void CSecretsManagerDaemonActor::CServer::fp_UpdateSemanticIDs(CStr const &_SemanticIDToRemove, CStr const &_SemanticIDToAdd)
	{
		if (_SemanticIDToAdd == _SemanticIDToRemove)
			return;

		if (_SemanticIDToRemove)
		{
			if (--mp_SemanticIDs[_SemanticIDToRemove] == 0)
			{
				TCSet<CStr> Permissions;
				Permissions[fg_Format("SecretsManager/SemanticID/{}", _SemanticIDToRemove)];
				mp_SemanticIDs.f_Remove(_SemanticIDToRemove);

				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, Permissions) > fg_DiscardResult();
			}
		}

		if (_SemanticIDToAdd)
		{
			if (++mp_SemanticIDs[_SemanticIDToAdd] == 1)
			{
				TCSet<CStr> Permissions;
				Permissions[fg_Format("SecretsManager/SemanticID/{}", _SemanticIDToAdd)];
				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
			}
		}
	}
	
	void CSecretsManagerDaemonActor::CServer::fp_WriteDatabase()
	{
		mp_DatabaseActor(&CSecretsManagerServerDatabase::f_WriteDatabase, fg_TempCopy(mp_Database)) > [](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to write database: {}", _Result.f_GetExceptionStr());
			}
		;
	}
}
