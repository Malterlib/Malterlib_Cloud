// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/NetworkTunnels>
#include <Mib/Cloud/NetworkTunnelsClient>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/VersionManager>

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppActor : public CDistributedAppActor
	{
		CCloudClientAppActor();
		~CCloudClientAppActor();
		
	protected:
		struct CCloudManagerAppManagerInfo
		{
			bool f_HasErrors() const;

			CStr m_HostName;
			CStr m_ProgramDirectory;
			CStr m_Environment;
			CStr m_LastConnectionError;
			CTime m_LastConnectionErrorTime;
			TCMap<CStr, CStr> m_OtherErrors;
			bool m_bActive;
		};

		enum ECloudManagerStatusFlag
		{
			ECloudManagerStatusFlag_AppManagers = DBit(0)
			, ECloudManagerStatusFlag_Applications = DBit(1)
		};
		
		TCFuture<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		TCFuture<void> fp_Initialize();

		void fp_ParseCommonOptions(NEncoding::CEJSON const &_Params);

		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_BackupManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Version Manager
		void fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_VersionManager_SubscribeToServers();
		
		TCFuture<uint32> fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Secrets manager
		void fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_SecretsManager_SubscribeToServers();
		static bool fsp_SecretsManager_GetID(CEJSON const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error);
		static NStr::CStr fsp_SecretsManager_CheckExpect(CSecretsManager::CSecret const &_Secret, NStr::CStr _Expect, bool _bBinaryAsBase64);

		template<typename tf_CType>
		TCFuture<uint32> fp_CommandLine_SecretsManager_Enumerate
		(
		 	CEJSON const &_Params
		 	, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunctionMovable
			<
				TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, TCOptional<CStrSecure> const &_pSemanticID, TCSet<CStrSecure> const &_Tags)
			>
		 	&&_fGetResult
			, TCFunctionMovable<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, NStr::CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
		);

		template<typename tf_CType>
		TCFuture<uint32> fp_CommandLine_SecretsManager_Get
			(
				CEJSON const &_Params
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			 	, TCFunctionMovable<TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> &&_fGetResult
				, TCFunctionMovable<NStr::CStr (tf_CType *pResult, TCSharedPointer<CCommandLineControl> const &_pCommandLine, NStr::CStr const &_Expect, bool _bBinaryAsBase64)> &&_fOnResult
			)
		;

		TCFuture<uint32> fp_CommandLine_SecretsManager_EnumerateSecrets(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecretBySemanticID(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetProperties(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecret(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCFuture<uint32> fp_CommandLine_SecretsManager_SetProperties(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_SetMetadata(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveMetadata(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_ChangeTags(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveSecret(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Upload(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Download(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Network tunnel
		void fp_NetworkTunnel_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_EnumTunnels(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_OpenTunnels(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<void> fp_NetworkTunnel_Init();
		TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> fp_NetworkTunnel_Filter(CEJSON const &_Params);

		// Cloud Manager
		void fp_CloudManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_CloudManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_CloudManager_Status(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine, ECloudManagerStatusFlag _Flags);
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_AppManagers
			(
				CEJSON const &_Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>>> const &_AppManagers
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_Applications
			(
				CEJSON const &_Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CCloudManager::CApplicationKey, CCloudManager::CApplicationInfo>>> const &_Applications
				, TCMap<CStr, CCloudManagerAppManagerInfo> const &_AppManagerInfos
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveAppManager(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		fp64 mp_Timeout = 0.0;

		TCVector<TCActor<CProcessLaunchActor>> mp_LaunchActors;

		TCVector<TCPromise<void>> mp_AppStopPromises;

		// Backup Manager
		TCTrustedActorSubscription<CBackupManager> mp_BackupManagers;
		CActorSubscription mp_DownloadBackupSubscription;

		// Version Manager
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagers;
		CVersionManagerHelper mp_VersionManagerHelper;

		// Secrets Manager
		TCTrustedActorSubscription<CSecretsManager> mp_SecretsManagers;
		CActorSubscription mp_UploadSubscription;

		// Network Tunnel
		TCVector<CActorSubscription> mp_TunnelSubscriptions;
		TCActor<CNetworkTunnelsClient> mp_TunnelsClient;

		// Cloud Manager
		TCTrustedActorSubscription<CCloudManager> mp_CloudManagers;
	};
}
