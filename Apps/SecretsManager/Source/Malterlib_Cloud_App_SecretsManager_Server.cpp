// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/File/File>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState, TCActor<CSecretsManagerServerDatabase> const &_DatabaseActor)
		: mp_AppState(_AppState)
		, mp_DatabaseActor(_DatabaseActor)
		, mp_FileActor(fg_ConstructActor<CFileActor>(fg_Construct("SecretsManager FileActor")))
		, mp_pCanDestroyFileActorTracker(fg_Construct())
	{
		fp_Init();
	}
	
	CSecretsManagerDaemonActor::CServer::~CServer()
	{
	}
	
#if DMibConfig_Tests_Enable
	TCFuture<CEJSON> CSecretsManagerDaemonActor::CServer::f_Test_Command(CStr const &_Command, CEJSON const &_Params)
	{
		if (_Command == "UploadInitialized")
			return mp_UploadInitialized[_Params.f_String()].f_Future();

		if (_Command == "UploadCompleted")
			return mp_UploadCompleted[_Params.f_String()].f_Future();

		if (_Command == "DownloadInitialized")
			return mp_DownloadInitialized[_Params.f_String()].f_Future();

		if (_Command == "DownloadCompleted")
			return mp_DownloadCompleted[_Params.f_String()].f_Future();

		if (_Command == "PreviousCommandCompleted")
			return fg_Explicit();

		if (_Command == "DelayDelete")
		{
			mp_bDelayDelete = true;
			return fg_Explicit();
		}

		if (_Command == "ReleaseDelete")
		{
			for (auto &Continutaion : mp_DelayDeletes)
				Continutaion.f_SetResult();
			mp_DelayDeletes.f_Clear();
			return fg_Explicit();
		}

		if (_Command == "DestroyWaitingForCanDestroy")
			return (mp_DestroyWaitingForCanDestroy = TCPromise<CEJSON>{})->f_Future();

		if (_Command == "SyncFileOperations")
			return mp_FileActor(&CSecretsManagerDaemonActor::CServer::CFileActor::f_SyncFileOperations);

		return DMibErrorInstance(fg_Format("Unhandled test command: {}", _Command));
	}
#endif

	void CSecretsManagerDaemonActor::CServer::fp_Init()
	{
		mp_DatabaseActor(&CSecretsManagerServerDatabase::f_ReadDatabase) > [this](TCAsyncResult<CSecretsDatabase> &&_Database)
			{
				if (!_Database)
				{
					DMibLogWithCategory(Mib/Cloud/SecretsManager, Error, "Failed to read database: {}", _Database.f_GetExceptionStr());
					return;
				}
				
				mp_Database = fg_Move(*_Database);
				for (auto const &SecretProperties : mp_Database.m_Secrets)
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
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCPromise<void> Promise;
		
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
		Permissions["SecretsManager/Command/RemoveSecret"];
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
			> Promise / [this, Promise](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Promise.f_SetResult();
			}
		;
		
		return Promise.f_MoveFuture();
	}
	
	
	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_Destroy()
	{
		DMibLogWithCategory(Mib/Cloud/SecretsManager, Debug, "Destroying protocol, uploads and downloads");

		TCPromise<void> Promise;
		TCActorResultVector<void> Results;

		mp_ProtocolInterface.m_Publication.f_Destroy() > Results.f_AddResult();
		
		for (auto &Upload : mp_Uploads)
			Upload.f_Destroy() > Results.f_AddResult();
		for (auto &Download : mp_Downloads)
			Download.f_Destroy() > Results.f_AddResult();

		Results.f_GetResults() > [=](auto &&)
			{
#if DMibConfig_Tests_Enable
				if (mp_DestroyWaitingForCanDestroy)
					mp_DestroyWaitingForCanDestroy->f_SetResult();
#endif
				auto pCanDestroy = fg_Move(mp_pCanDestroyFileActorTracker);
				DMibLogWithCategory(Mib/Cloud/SecretsManager, Debug, "Uploads and downloads destroyed, wating for can destroy file actor");
				pCanDestroy->f_Future() > [=](auto &&)
					{
						TCFuture<void> FileActorFuture;
						if (mp_FileActor)
							FileActorFuture = mp_FileActor->f_Destroy();
						else
							FileActorFuture = fg_Explicit();

						TCFuture<void> DatabaseFuture;
						if (mp_DatabaseActor)
							DatabaseFuture = mp_DatabaseActor->f_Destroy();
						else
							DatabaseFuture = fg_Explicit();

						DMibLogWithCategory(Mib/Cloud/SecretsManager, Debug, "Can destroy file actor, waiting for file and database actor destroy");
						fg_Move(FileActorFuture) + fg_Move(DatabaseFuture) > [=](auto &&, auto &&)
							{
								DMibLogWithCategory(Mib/Cloud/SecretsManager, Debug, "Destroying protocol interface");
								mp_ProtocolInterface.f_Destroy() > [=](auto &&)
									{
										DMibLogWithCategory(Mib/Cloud/SecretsManager, Debug, "Destroy finished");
										Promise.f_SetResult();
									}
								;
							}
						;
					}
				;
			}
		;

		return Promise.f_MoveFuture();
	}

	namespace
	{
		CStr fg_GetPermissionsDescription(CStr const &_SemanticID, TCSet<CStrSecure> const &_Tags)
		{
			CStr Description;

			if (!_SemanticID.f_IsEmpty() && !_Tags.f_IsEmpty())
				Description = "Access to semantic ID '{}' and tags {vs}"_f << _SemanticID << _Tags;
			else if (!_SemanticID.f_IsEmpty())
				Description = "Access to semantic ID '{}'"_f << _SemanticID;
			else if (!_Tags.f_IsEmpty())
				Description = "Access to tags {vs}"_f << _Tags;

			return Description;
		}
	}

	// This function is similar to fsp_AddPermissionQueryIndexedBySecretID below.
	//
	// fsp_AddPermissionQueryIndexedByPermission adds _several_ queries with wildcard patterns where _ALL_ have to be fulfilled.
	void CSecretsManagerDaemonActor::CServer::fsp_AddPermissionQueryIndexedByPermission
		(
			char const *_ReadWrite
			, CStr const &_SemanticID
			, TCSet<CStrSecure> const &_Tags
			, TCMap<CStr, TCVector<CPermissionQuery>> &o_Permissions
		)
	{
		CStr SemanticPart{_SemanticID ? fg_Format("SemanticID/{}", _SemanticID) : "NoSemanticID"};
		CStr Description = fg_GetPermissionsDescription(_SemanticID, _Tags);

		if (_Tags.f_IsEmpty())
		{
			auto PermissionString = fg_Format("SecretsManager/{}/{}/NoTag", _ReadWrite, SemanticPart);
			o_Permissions[PermissionString] = {CPermissionQuery{{PermissionString}}.f_Wildcard(true).f_Description(Description)};
		}
		else
		{
			for (auto const &Tag : _Tags)
			{
				auto PermissionString = fg_Format("SecretsManager/{}/{}/Tag/{}", _ReadWrite, SemanticPart, Tag);
				o_Permissions[PermissionString] = {CPermissionQuery{{PermissionString}}.f_Wildcard(true).f_Description(Description)};
				Description.f_Clear();
			}
		}
	}

	// This function is similar to fsp_AddPermissionQueryIndexedByPermission above.
	//
	// fsp_AddPermissionQueryIndexedBySecretID adds _one_ permission query with _several_ wildcards patterns where _ALL_ have to be fulfilled.
	void CSecretsManagerDaemonActor::CServer::fsp_AddPermissionQueryIndexedBySecretID
		(
			CSecretsManager::CSecretID const &_ID
			, char const *_ReadWrite
			, CStr const &_SemanticID
			, TCSet<CStrSecure> const &_Tags
			, TCMap<CStr, TCVector<CPermissionQuery>> &o_Permissions
		)
	{
		auto &PermissionCollection = o_Permissions[CStr::fs_ToStr(_ID)];
		CStr SemanticPart{ _SemanticID ? fg_Format("SemanticID/{}", _SemanticID) : "NoSemanticID"};
		CStr Description = fg_GetPermissionsDescription(_SemanticID, _Tags);

		if (_Tags.f_IsEmpty())
		{
			auto PermissionString = fg_Format("SecretsManager/{}/{}/NoTag", _ReadWrite, SemanticPart);
			PermissionCollection.f_Insert({CPermissionQuery{{fg_Move(PermissionString)}}.f_Wildcard(true).f_Description(Description)});
		}
		else
		{
			for (auto const &Tag : _Tags)
			{
				CStr PermissionString = "SecretsManager/{}/{}/Tag/{}"_f << _ReadWrite << SemanticPart << Tag;
				PermissionCollection.f_Insert({CPermissionQuery{{fg_Move(PermissionString)}}.f_Wildcard(true).f_Description(Description)});
				Description.f_Clear();
			}
		}
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
		
		for (auto const &Tag : _TagsToRemove)
		{
			if (InBoth.f_FindEqual(Tag))
				continue;

			// Take care: Since we allow "removal" of tags not on the property we should only decrease the count if the count is in the set.
			// Otherwise we risk getting negative counts and ending up with a too low count when the tags is added later
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
			if (InBoth.f_FindEqual(Tag))
				continue;

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

	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_RemoveFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor)
	{
		TCPromise<void> Promise;
		CStr FileName{"{}/SecretsManagerFiles/{}"_f << mp_AppState.m_RootDirectory << _FileName};
		mp_FileActor(&CFileActor::f_Delete, FileName) > [Promise, _Auditor, pCanDestroyTracker = mp_pCanDestroyFileActorTracker](TCAsyncResult<void> &&_Result) mutable
			{
				if (!_Result)
				{
					Promise.f_SetException
						(
						 	_Auditor.f_Exception({"Internal error. Check SecretsManager.log for details.", fg_Format("File removal failed: {}", _Result.f_GetExceptionStr())})
						)
					;
				}
				else
					Promise.f_SetResult();
			}
		;
		return Promise.f_MoveFuture();
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_RemoveUnreferencedFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor)
	{
		if (!_FileName)
			return fg_Explicit();

		if (auto *pReservedFile = mp_ReservedFiles.f_FindEqual(_FileName))
		{
			DCheck(!pReservedFile->m_fPendingDelete);
			TCPromise<void> Promise;
			pReservedFile->m_fPendingDelete = [=, pCanDestroyTracker = mp_pCanDestroyFileActorTracker]()
				{
#if DMibConfig_Tests_Enable
					if (mp_bDelayDelete)
					{

						// This is used to test shutdown. We wait here while the test checks that the file has been deleted
						// and that the future from the secrets manager destruction has not resloved set yet
 						mp_DelayDeletes.f_Insert().f_Future() > [=, pCanDestroyTracker = pCanDestroyTracker](auto &&)
							{
								fp_RemoveFile(_FileName, _Auditor) > Promise / [pCanDestroyTracker, Promise]()
									{
										Promise.f_SetResult();
									}
								;
							}
						;
					}
					else
#endif
					{
						fp_RemoveFile(_FileName, _Auditor) > Promise / [pCanDestroyTracker, Promise]()
							{
								Promise.f_SetResult();
							}
						;
					}
				}
			;
			return Promise.f_MoveFuture();
		}
		else
			return fp_RemoveFile(_FileName, _Auditor);
	}

	CActorSubscription CSecretsManagerDaemonActor::CServer::fp_ReserveFile(CStr const &_FileName)
	{
		if (!_FileName)
			DMibError("Empty file name");

		++mp_ReservedFiles[_FileName].m_RefCount;
		return NConcurrency::g_ActorSubscription / [this, _FileName]
			{
				if (auto *pReservedFile = mp_ReservedFiles.f_FindEqual(_FileName))
				{
					if (--pReservedFile->m_RefCount == 0)
					{
						if (pReservedFile->m_fPendingDelete)
							pReservedFile->m_fPendingDelete();

						mp_ReservedFiles.f_Remove(pReservedFile);
					}
				}
			}
		;
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

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CFileActor::f_Delete(NStr::CStr const &_File)
	{
		return TCFuture<void>::fs_RunProtected<CExceptionFile>() / [&]()
			{
				if (CFile::fs_FileExists(_File))
					CFile::fs_DeleteFile(_File);
			}
		;
	}

#if DMibConfig_Tests_Enable
	TCFuture<CEJSON> CSecretsManagerDaemonActor::CServer::CFileActor::f_SyncFileOperations()
	{
		// This function is used for debugging concurrent operations.
		return fg_Explicit();
	}

	TCFuture<CEJSON> CSecretsManagerDaemonActor::CServer::f_SyncFileOperations()
	{
		return mp_FileActor(&CFileActor::f_SyncFileOperations);
	}

#endif

}
