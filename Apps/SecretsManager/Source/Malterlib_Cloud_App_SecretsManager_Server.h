// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
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

		struct CSecretsManagerImplementation : public CSecretsManager
		{
			TCContinuation<TCSet<CSecretID>> f_EnumerateSecrets
				(
					TCOptional<CStrSecure> const &_SemanticID
					, TCSet<CStrSecure> const &_TagsExclusive
				) override
			;
			TCContinuation<void> f_SetSecretProperties(CSecretID &&_ID, CSecretProperties &&_Secret) override;
			TCContinuation<CSecretProperties> f_GetSecretProperties(CSecretID &&_ID) override;
			TCContinuation<CSecret> f_GetSecret(CSecretID &&_ID) override;
			TCContinuation<CSecret> f_GetSecretBySemanticID(CStrSecure const &_SemanticID, TCSet<CStrSecure> const &_TagsExclusive) override;
			TCContinuation<CActorSubscription> f_DownloadFile(CFileTransferContext &&_TransferContext) override;
			TCContinuation<void> f_ModifyTags
				(
					CSecretID &&_ID
					, TCSet<CStrSecure> &&_TagsToRemove
					, TCSet<CStrSecure> &&_TagsToAdd
				) override
			;
			TCContinuation<void> f_SetMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey, CEJSON &&_Metadata) override;
			TCContinuation<void> f_RemoveMetadata(CSecretID &&_ID, CStrSecure const &_MetadataKey) override;
			TCContinuation<void> f_RemoveSecret(CSecretID &&_ID) override;
			auto f_UploadFile(TCActorFunctorWithID<TCContinuation<void> (CFileTransferContext &&_TransferContext)> &&_fOnNotification)
				-> TCContinuation<TCActorSubscriptionWithID<>> override
			;
			CServer *m_pThis;
		};
		
	private:
		TCContinuation<void> fp_Destroy() override;
		void fp_Init();
		void fp_Publish();
		
		TCContinuation<void> fp_SetupPermissions();
		bool fp_HasPermission(char const *_ReadWrite, NStr::CStr const &_SemanticID, TCSet<CStrSecure> const &_Tags, NStr::CStr &_oPermission);
		void fp_UpdateTags(TCSet<CStrSecure> const &_TagsToRemove,TCSet<CStrSecure> const &_TagsToAdd);
		void fp_UpdateSemanticIDs(NStr::CStr const &_SemanticIDToRemove, NStr::CStr const &_SemanticIDToAdd);
		void fp_WriteDatabase();

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;
		TCDelegatedActorInterface<CSecretsManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;
		CTrustedPermissionSubscription mp_Permissions;

		TCActor<CSecretsManagerServerDatabase> mp_DatabaseActor;
		CSecretsManagerServerDatabase::CDatabase mp_Database;
		TCMap<NStr::CStr, uint32> mp_Tags;
		TCMap<NStr::CStr, uint32> mp_SemanticIDs;
	};
}

