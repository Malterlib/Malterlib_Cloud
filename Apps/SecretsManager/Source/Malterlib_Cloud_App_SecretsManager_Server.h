// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/KeyManager>
#include <Mib/Container/Map>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Database.h"

namespace NMib::NCloud::NSecretsManager
{
	struct CSecretsManagerDaemonActor::CServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;
		
		CServer(CDistributedAppState &_AppState, TCActor<CSecretsManagerServerDatabase> const &_DatabaseActor);
		~CServer();

#if DMibConfig_Tests_Enable
		TCContinuation<NEncoding::CEJSON> f_Test_Command(CStr const &_Command, CEJSON const &_Params);
#endif

		struct CSecretsManagerImplementation : public CSecretsManager
		{
			TCContinuation<TCSet<CSecretID>> f_EnumerateSecrets(TCOptional<CStrSecure> const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive) override;
			TCContinuation<void> f_SetSecretProperties(CSecretID &&_ID, CSecretProperties &&_Secret) override;
			TCContinuation<CSecretProperties> f_GetSecretProperties(CSecretID &&_ID) override;
			TCContinuation<CSecret> f_GetSecret(CSecretID &&_ID) override;
			TCContinuation<CSecret> f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive) override;
			TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>> f_DownloadFile(CSecretID &&_ID, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription) override;
			TCContinuation<void> f_ModifyTags(CSecretID &&_ID, TCSet<CStrSecure> &&_TagsToRemove, TCSet<CStrSecure> &&_TagsToAdd) override;
			TCContinuation<void> f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata) override;
			TCContinuation<void> f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey) override;
			TCContinuation<void> f_RemoveSecret(CSecretID &&_ID) override;
			TCContinuation<NConcurrency::TCActorFunctorWithID<TCContinuation<void> ()>> f_UploadFile
				(
				 	CSecretID &&_ID
				 	, NStr::CStrSecure const &_FileName
				 	, TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_Uploader
				 ) override
			;

			CServer *m_pThis;
		};

		struct CDownload
		{
			CDownload();
			~CDownload();
			TCContinuation<void> f_Destroy();

			TCDistributedActor<CDirectorySyncSend> m_DirectorySyncSend;
			NConcurrency::TCActorSubscriptionWithID<> m_Subscription;
			NConcurrency::CActorSubscription m_FileSubscription;
		};

		struct CUpload
		{
			CUpload();
			~CUpload();
			TCContinuation<void> f_Destroy();

			TCActor<CDirectorySyncReceive> m_DirectorySyncReceive;
		};

		struct CReservedFile
		{
			mint m_RefCount = 0;
			TCFunctionMutable<void ()> m_fPendingDelete;
		};

		class CFileActor : public CActor
		{
		public:
			using CActorHolder = NConcurrency::CSeparateThreadActorHolder;

			NConcurrency::TCContinuation<void> f_Delete(NStr::CStr const &_File);
#if DMibConfig_Tests_Enable
			NConcurrency::TCContinuation<CEJSON> f_SyncFileOperations();
#endif
		};

	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_Init();
		void fp_Publish();
		
		TCContinuation<void> fp_SetupPermissions();
		static void fsp_AddPermissionQueryIndexedByPermission
			(
				char const *_ReadWrite
				, NStr::CStr const &_SemanticID
				, TCSet<NStr::CStrSecure> const &_Tags
				, TCMap<NStr::CStr, TCVector<CPermissions>> &o_Permissions
			)
		;
		static void fsp_AddPermissionQueryIndexedBySecretID
			(
				CSecretsManager::CSecretID const &_ID
				, char const *_ReadWrite
				, NStr::CStr const &_SemanticID
				, TCSet<NStr::CStrSecure> const &_Tags
				, TCMap<NStr::CStr, TCVector<CPermissions>> &o_Permissions
			)
		;
		static void fsp_AddPermissionsForMatchingSecrets
			(
				TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
				, TCOptional<CStrSecure> const &_SemanticID
				, TCSet<CStrSecure> const &_TagsExclusive
			 	, NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissions>> &o_Permissions
			)
		;
		void fp_UpdateTags(TCSet<CStrSecure> const &_TagsToRemove,TCSet<CStrSecure> const &_TagsToAdd);
		void fp_UpdateSemanticIDs(NStr::CStr const &_SemanticIDToRemove, NStr::CStr const &_SemanticIDToAdd);
		TCContinuation<void> fp_RemoveFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor);
		CActorSubscription fp_ReserveFile(CStr const &_FileName);
		TCContinuation<void> fp_RemoveUnreferencedFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor);
		void fp_WriteDatabase();
		
#if DMibConfig_Tests_Enable
		NConcurrency::TCContinuation<CEJSON> f_SyncFileOperations();
#endif

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyFileActorTracker;
		TCDelegatedActorInterface<CSecretsManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;
		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CSecretsManagerServerDatabase> mp_DatabaseActor;
		TCActor<CFileActor> mp_FileActor;
		CSecretsDatabase mp_Database;
		TCMap<NStr::CStr, uint32> mp_Tags;
		TCMap<NStr::CStr, uint32> mp_SemanticIDs;

		TCMap<NStr::CStr, CDownload> mp_Downloads;
		TCMap<NStr::CStr, CUpload> mp_Uploads;
		TCMap<NStr::CStr, CReservedFile> mp_ReservedFiles;

#if DMibConfig_Tests_Enable
		// The vectors below are used to synchronize "threads" during LaunchInProcess tests so certain cases can be provoked
		TCMap<NStr::CStr, TCContinuation<CEJSON>> mp_UploadInitialized;
		TCMap<NStr::CStr, TCContinuation<CEJSON>> mp_UploadCompleted;
		TCMap<NStr::CStr, TCContinuation<CEJSON>> mp_DownloadInitialized;
		TCMap<NStr::CStr, TCContinuation<CEJSON>> mp_DownloadCompleted;
		TCOptional<TCContinuation<CEJSON>> mp_DestroyWaitingForCanDestroy;
		bool mp_bDelayDelete = false;
		TCVector<TCContinuation<void>> mp_DelayDeletes;
#endif
	};
}

