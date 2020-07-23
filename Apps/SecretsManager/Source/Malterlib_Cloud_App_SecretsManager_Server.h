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

		struct CSecretsManagerImplementation : public CSecretsManager
		{
			TCFuture<TCSet<CSecretID>> f_EnumerateSecrets(TCOptional<CStrSecure> const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive) override;
			TCFuture<void> f_SetSecretProperties(CSecretID &&_ID, CSecretProperties &&_Secret) override;
			TCFuture<CSecretProperties> f_GetSecretProperties(CSecretID &&_ID) override;
			TCFuture<CSecret> f_GetSecret(CSecretID &&_ID) override;
			TCFuture<CSecret> f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive) override;
			TCFuture<TCDistributedActorInterfaceWithID<CDirectorySyncClient>> f_DownloadFile(CSecretID &&_ID, TCActorSubscriptionWithID<> &&_Subscription) override;
			TCFuture<void> f_ModifyTags(CSecretID &&_ID, TCSet<CStrSecure> &&_TagsToRemove, TCSet<CStrSecure> &&_TagsToAdd) override;
			TCFuture<void> f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata) override;
			TCFuture<void> f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey) override;
			TCFuture<void> f_RemoveSecret(CSecretID &&_ID) override;
			TCFuture<TCActorFunctorWithID<TCFuture<void> ()>> f_UploadFile
				(
					CSecretID &&_ID
					, CStrSecure const &_FileName
					, TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_Uploader
				) override
			;
			TCFuture<TCActorSubscriptionWithID<>> f_SubscribeToChanges(CSubscribeToChanges &&_Params) override;

			CServer *m_pThis;
#			ifdef DMibDebug
				CEmpty self; // Hide dangerous self
#			endif
		};

		struct CDownload
		{
			CDownload();
			CDownload(CDownload &&);
			~CDownload();
			TCFuture<void> f_Destroy();

			TCDistributedActor<CDirectorySyncSend> m_DirectorySyncSend;
			TCActorSubscriptionWithID<> m_Subscription;
			CActorSubscription m_FileSubscription;
		};

		struct CUpload
		{
			CUpload();
			CUpload(CUpload &&);
			~CUpload();
			TCFuture<void> f_Destroy();

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
			using CActorHolder = CSeparateThreadActorHolder;

			TCFuture<void> f_Delete(CStr const &_File);
#if DMibConfig_Tests_Enable
			TCFuture<CEJSON> f_SyncFileOperations();
#endif
		};

		struct CChangeSubscription
		{
			CStr const &f_GetSubscriptionID() const
			{
				return TCMap<CStr, CChangeSubscription>::fs_GetKey(*this);
			}
			CCallingHostInfo m_CallingHostInfo;

			CSecretsManager::CSubscribeToChanges m_SubscriptionParams;

			void f_SendChanges(CSecretsManager::CSecretChanges &&_Changes) const;
		};

		CServer(CDistributedAppState &_AppState, TCActor<CSecretsManagerServerDatabase> const &_DatabaseActor);
		~CServer();

		TCFuture<void> f_Init();

#if DMibConfig_Tests_Enable
		TCFuture<CEJSON> f_Test_Command(CStr const &_Command, CEJSON const &_Params);
#endif
		static bool fs_MatchSecret(CSecretPropertiesInternal const &_SecretProperties, TCOptional<CStrSecure> const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive);

	private:
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();

		TCFuture<void> fp_SetupPermissions();

		static void fsp_AddPermissionQueryIndexedByPermission
			(
				char const *_ReadWrite
				, CStr const &_SemanticID
				, TCSet<CStrSecure> const &_Tags
				, TCMap<CStr, TCVector<CPermissionQuery>> &o_Permissions
			)
		;
		static void fsp_AddPermissionQueryIndexedBySecretID
			(
				CSecretsManager::CSecretID const &_ID
				, char const *_ReadWrite
				, CStr const &_SemanticID
				, TCSet<CStrSecure> const &_Tags
				, TCMap<CStr, TCVector<CPermissionQuery>> &o_Permissions
			)
		;
		static void fsp_AddPermissionsForMatchingSecrets
			(
				TCMap<CSecretsManager::CSecretID, CSecretPropertiesInternal> &_Secrets
				, TCOptional<CStrSecure> const &_SemanticID
				, TCSet<CStrSecure> const &_TagsExclusive
			 	, TCMap<CStr, TCVector<CPermissionQuery>> &o_Permissions
			)
		;
		void fp_UpdateTags(TCSet<CStrSecure> const &_TagsToRemove,TCSet<CStrSecure> const &_TagsToAdd);
		void fp_UpdateSemanticIDs(CStr const &_SemanticIDToRemove, CStr const &_SemanticIDToAdd);
		TCFuture<void> fp_RemoveFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor);
		CActorSubscription fp_ReserveFile(CStr const &_FileName);
		TCFuture<void> fp_RemoveUnreferencedFile(CStr const &_FileName, CDistributedAppAuditor const &_Auditor);
		TCFuture<void> fp_WriteDatabase();

#if DMibConfig_Tests_Enable
		TCFuture<CEJSON> f_SyncFileOperations();
#endif

		TCFuture<void> fp_SendSubscriptionInitial(CStr const &_SubscriptionID);
		void fp_UpdateSubscriptionsForChangedPermissions(CPermissionIdentifiers const &_Identity);
		void fp_SecretUpdated(CSecretPropertiesInternal const &_SecretProperties, bool _bRemoved);
		bool fp_SecretMatchesSubscription(CChangeSubscription const &_Subscription, CSecretPropertiesInternal const &_SecretProperties);

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyFileActorTracker;
		TCDistributedActorInstance<CSecretsManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;
		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CSecretsManagerServerDatabase> mp_DatabaseActor;
		TCActor<CFileActor> mp_FileActor;
		CSecretsDatabase mp_Database;
		TCMap<CStr, uint32> mp_Tags;
		TCMap<CStr, uint32> mp_SemanticIDs;

		TCMap<CStr, CDownload> mp_Downloads;
		TCMap<CStr, CUpload> mp_Uploads;
		TCMap<CStr, CReservedFile> mp_ReservedFiles;

		TCMap<CStr, CChangeSubscription> mp_ChangeSubscriptions;

#if DMibConfig_Tests_Enable
		// The vectors below are used to synchronize "threads" during LaunchInProcess tests so certain cases can be provoked
		TCMap<CStr, TCPromise<CEJSON>> mp_UploadInitialized;
		TCMap<CStr, TCPromise<CEJSON>> mp_UploadCompleted;
		TCMap<CStr, TCPromise<CEJSON>> mp_DownloadInitialized;
		TCMap<CStr, TCPromise<CEJSON>> mp_DownloadCompleted;
		TCOptional<TCPromise<CEJSON>> mp_DestroyWaitingForCanDestroy;
		bool mp_bDelayDelete = false;
		TCVector<TCPromise<void>> mp_DelayDeletes;
#endif
	};
}

