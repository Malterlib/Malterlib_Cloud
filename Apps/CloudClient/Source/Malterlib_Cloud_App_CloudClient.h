// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Cloud/SecretsManager>

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppActor : public CDistributedAppActor
	{
		CCloudClientAppActor();
		~CCloudClientAppActor();
		
	protected:
		
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		TCContinuation<void> fp_Initialize();

		void fp_ParseCommonOptions(NEncoding::CEJSON const &_Params);

		
		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCContinuation<void> fp_BackupManager_SubscribeToServers();
		TCContinuation<uint32> fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Version Manager
		void fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCContinuation<void> fp_VersionManager_SubscribeToServers();
		
		TCContinuation<uint32> fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Secrets manager
		void fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCContinuation<void> fp_SecretsManager_SubscribeToServers();
		static bool fsp_SecretsManager_GetID(CEJSON const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error);
		static NStr::CStr fsp_SecretsManager_CheckExpect(CSecretsManager::CSecret const &_Secret, NStr::CStr _Expect, bool _bBinaryAsBase64);

		template<typename tf_CType>
		TCContinuation<uint32> fp_CommandLine_SecretsManager_Enumerate
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunction
			<
				TCContinuation<tf_CType>(TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_pSemanticID, TCSet<CStrSecure> const &_Tags)
			> &&_fGetResult
			, TCFunction<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, NStr::CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
		);

		template<typename tf_CType>
		TCContinuation<uint32> fp_CommandLine_SecretsManager_Get
			(
				CEJSON const &_Params
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			 	, TCFunction<TCContinuation<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> &&_fGetResult
				, TCFunction<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, NStr::CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
			)
		;

		TCContinuation<uint32> fp_CommandLine_SecretsManager_EnumerateSecrets(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_GetSecretBySemanticID(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_GetProperties(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_GetSecret(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCContinuation<uint32> fp_CommandLine_SecretsManager_SetProperties(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_SetMetadata(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_RemoveMetadata(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_ChangeTags(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_RemoveSecret(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_Upload(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCContinuation<uint32> fp_CommandLine_SecretsManager_Download(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		fp64 mp_Timeout = 0.0;
		
		// Backup Manager
		TCTrustedActorSubscription<CBackupManager> mp_BackupManagers;
		CActorSubscription mp_DownloadBackupSubscription;

		// Version Manager
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagers;
		CVersionManagerHelper mp_VersionManagerHelper;

		// Secrets Manager
		TCTrustedActorSubscription<CSecretsManager> mp_SecretsManagers;
		CActorSubscription mp_UploadSubscription;
	};
}
