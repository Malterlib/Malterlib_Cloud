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
		
		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override;
		TCFuture<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		TCFuture<void> fp_Initialize();

		void fp_ParseCommonOptions(NEncoding::CEJSONSorted const &_Params);

		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_BackupManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackupSources(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackups(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_DownloadBackup(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Version Manager
		void fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_VersionManager_SubscribeToServers();
		
		TCFuture<uint32> fp_CommandLine_VersionManager_ListApplications(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ListVersions(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_UploadVersion(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_DownloadVersion(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ChangeTags(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Secrets manager
		void fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_SecretsManager_SubscribeToServers();
		static bool fsp_SecretsManager_GetID(CEJSONSorted const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error);
		static NStr::CStr fsp_SecretsManager_CheckExpectedFormat
			(
				CSecretsManager::CSecret const &_Secret
				, NStr::CStr const &_ExpectedFormat
				, bool _bBinaryAsBase64
				, TCOptional<CStrSecure> const &_MapKey
			)
		;
		static NStr::CStr fsp_SecretsManager_OutputSecret
			(
				NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine
				, CSecretsManager::CSecret const &_Secret
				, bool _bBinaryAsBase64
				, TCOptional<CStrSecure> const &_MapKey
			)
		;

		template<typename tf_CType>
		TCFuture<uint32> fp_CommandLine_SecretsManager_EnumerateImpl
		(
			CEJSONSorted const &_Params
			, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			, TCFunctionMovable
			<
				TCFuture<tf_CType>
				(
					TCDistributedActor<CSecretsManager> const &_Actor
					, TCOptional<CStrSecure> const &_SemanticID
					, TCOptional<CStrSecure> const &_Name
					, TCSet<CStrSecure> const &_Tags
				)
			>
			&&_fGetResult
			, TCFunctionMovable
			<
				NStr::CStr
				(
					tf_CType const &_Result
					, TCSharedPointer<CCommandLineControl> const &_pCommandLine
					, NStr::CStr const &_ExpectedFormat
					, TCOptional<CStrSecure> const &_MapKey
					, bool _bBinaryAsBase64
				)
			> &&_fOnResult
		);

		template<typename tf_CType>
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetImpl
			(
				CEJSONSorted const &_Params
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
				, TCFunctionMovable<TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> &&_fGetResult
				, TCFunctionMovable
				<
					NStr::CStr
					(
						tf_CType const &_Result
						, TCSharedPointer<CCommandLineControl> const &_pCommandLine
						, NStr::CStr const &_ExpectedFormat
						, TCOptional<CStrSecure> const &_MapKey
						, bool _bBinaryAsBase64
					)
				> &&_fOnResult
			)
		;

		TCFuture<uint32> fp_CommandLine_SecretsManager_EnumerateSecrets(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecretBySemanticID(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetProperties(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecret(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCFuture<uint32> fp_CommandLine_SecretsManager_SetProperties(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_SetMetadata(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveMetadata(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_ChangeTags(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveSecret(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Upload(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Download(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		// Network tunnel
		void fp_NetworkTunnel_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_EnumTunnels(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_OpenTunnels(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<void> fp_NetworkTunnel_Init();
		TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> fp_NetworkTunnel_Filter(CEJSONSorted const &_Params);

		// Cloud Manager
		void fp_CloudManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_CloudManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_CloudManager_Status(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine, ECloudManagerStatusFlag _Flags);
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_AppManagers
			(
				CEJSONSorted const &_Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>>> const &_AppManagers
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_Applications
			(
				CEJSONSorted const &_Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CCloudManager::CApplicationKey, CCloudManager::CApplicationInfo>>> const &_Applications
				, TCMap<CStr, CCloudManagerAppManagerInfo> const &_AppManagerInfos
				, TCSharedPointer<CCommandLineControl> const &_pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveAppManager(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveSensor(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveLog(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_ExpectedOsVersionList(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_ExpectedOsVersionSet(CEJSONSorted const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine);

		TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>>> fp_CommandLine_CloudManager_GetSensorReaders(CStr const &_Host);
		TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> fp_CommandLine_CloudManager_GetAggregatedSensors
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
				, CDistributedAppSensorReader_SensorFilter const &_Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> fp_CommandLine_CloudManager_GetAggregatedSensorStatus
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
				, CDistributedAppSensorReader_SensorStatusFilter const &_Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> fp_CommandLine_CloudManager_GetAggregatedSensorReadings
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> const &_pSensorReaders
				, CDistributedAppSensorReader_SensorReadingFilter const &_Filter
			)
		;

		TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>>> fp_CommandLine_CloudManager_GetLogReaders(CStr const &_Host);
		TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>> fp_CommandLine_CloudManager_GetAggregatedLogs
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> const &_pLogReaders
				, CDistributedAppLogReader_LogFilter const &_Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>> fp_CommandLine_CloudManager_GetAggregatedLogEntries
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> const &_pLogReaders
				, CDistributedAppLogReader_LogEntryFilter const &_Filter
			)
		;

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
