// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/DebugManager>
#include <Mib/Cloud/DebugManagerHelper>
#include <Mib/Cloud/NetworkTunnels>
#include <Mib/Cloud/NetworkTunnelsClient>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Process/ProcessLaunchActor>

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppActor : public CDistributedAppActor
	{
		CCloudClientAppActor();
		~CCloudClientAppActor();

	protected:
		struct CCloudManagerAppManagerInfo
		{
			bool f_HasErrors(CTime const &_Now) const;
			bool f_IsPaused(CTime const &_Now) const;

			CStr m_HostName;
			CStr m_ProgramDirectory;
			CStr m_Environment;
			CStr m_LastConnectionError;
			CTime m_LastConnectionErrorTime;
			CTime m_LastSeen;
			TCMap<CStr, CStr> m_OtherErrors;
			bool m_bActive = false;
			fp32 m_PauseReportingFor = fp32::fs_QNan();
		};

		enum ECloudManagerStatusFlag
		{
			ECloudManagerStatusFlag_AppManagers = DBit(0)
			, ECloudManagerStatusFlag_Applications = DBit(1)
		};

		TCFuture<void> fp_StartApp(NEncoding::CEJsonSorted const _Params) override;
		TCFuture<void> fp_StopApp() override;
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_DestroyAll();
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;

		TCFuture<void> fp_Initialize();

		void fp_ParseCommonOptions(NEncoding::CEJsonSorted const &_Params);

		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_BackupManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackupSources(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_ListBackups(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_BackupManager_DownloadBackup(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		// Version Manager
		void fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_VersionManager_SubscribeToServers();

		TCFuture<uint32> fp_CommandLine_VersionManager_ListApplications(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ListVersions(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_UploadVersion(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_DownloadVersion(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_ChangeTags(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_VersionManager_CopyVersions(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		// Secrets manager
		void fp_SecretsManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_SecretsManager_SubscribeToServers();
		static bool fsp_SecretsManager_GetID(CEJsonSorted const &_Params, CSecretsManager::CSecretID &o_ID, CStr &o_Error);
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
			CEJsonSorted const _Params
			, TCSharedPointer<CCommandLineControl> _pCommandLine
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
			_fGetResult
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
			> _fOnResult
		);

		template<typename tf_CType>
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetImpl
			(
				CEJsonSorted const _Params
				, TCSharedPointer<CCommandLineControl> _pCommandLine
				, TCFunctionMovable<TCFuture<tf_CType> (TCDistributedActor<CSecretsManager> const &_Actor, CSecretsManager::CSecretID const &_ID)> _fGetResult
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
				> _fOnResult
			)
		;

		TCFuture<uint32> fp_CommandLine_SecretsManager_EnumerateSecrets(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecretBySemanticID(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetProperties(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_GetSecret(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_SecretsManager_SetProperties(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_SetMetadata(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveMetadata(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_ChangeTags(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_RemoveSecret(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Upload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_SecretsManager_Download(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		// Network tunnel
		void fp_NetworkTunnel_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_EnumTunnels(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_NetworkTunnel_OpenTunnels(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<void> fp_NetworkTunnel_Init();
		TCFuture<TCMap<CStr, TCMap<ICNetworkTunnels::CNetworkTunnelName, ICNetworkTunnels::CNetworkTunnel>>> fp_NetworkTunnel_Filter(CEJsonSorted const _Params);

		// Cloud Manager
		void fp_CloudManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_CloudManager_SubscribeToServers();
		TCFuture<uint32> fp_CommandLine_CloudManager_Status(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine, ECloudManagerStatusFlag _Flags);
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_AppManagers
			(
				CEJsonSorted const _Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CCloudManager::CAppManagerDynamicInfo>>> _AppManagers
				, TCSharedPointer<CCommandLineControl> _pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_Status_Applications
			(
				CEJsonSorted const _Params
				, TCMap<CHostInfo, TCAsyncResult<TCMap<CCloudManager::CApplicationKey, CCloudManager::CApplicationInfo>>> _Applications
				, TCMap<CStr, CCloudManagerAppManagerInfo> _AppManagerInfos
				, TCSharedPointer<CCommandLineControl> _pCommandLine
			)
		;
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveAppManager(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveSensor(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_RemoveLog(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_SnoozeSensor(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_ExpectedOsVersionList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_CloudManager_ExpectedOsVersionSet(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>>> fp_CommandLine_CloudManager_GetSensorReaders(CStr _Host);
		TCAsyncGenerator<TCVector<CDistributedAppSensorReporter::CSensorInfo>> fp_CommandLine_CloudManager_GetAggregatedSensors
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> _pSensorReaders
				, CDistributedAppSensorReader_SensorFilter _Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> fp_CommandLine_CloudManager_GetAggregatedSensorStatus
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> _pSensorReaders
				, CDistributedAppSensorReader_SensorStatusFilter _Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppSensorReader_SensorKeyAndReading>> fp_CommandLine_CloudManager_GetAggregatedSensorReadings
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppSensorReader>>> _pSensorReaders
				, CDistributedAppSensorReader_SensorReadingFilter _Filter
			)
		;

		TCFuture<TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>>> fp_CommandLine_CloudManager_GetLogReaders(CStr _Host);
		TCAsyncGenerator<TCVector<CDistributedAppLogReporter::CLogInfo>> fp_CommandLine_CloudManager_GetAggregatedLogs
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> _pLogReaders
				, CDistributedAppLogReader_LogFilter _Filter
			)
		;
		TCAsyncGenerator<TCVector<CDistributedAppLogReader_LogKeyAndEntry>> fp_CommandLine_CloudManager_GetAggregatedLogEntries
			(
				TCSharedPointer<TCMap<CHostInfo, TCDistributedActorInterfaceWithID<CDistributedAppLogReader>>> _pLogReaders
				, CDistributedAppLogReader_LogEntryFilter _Filter
			)
		;

		// Debug Manager
		void fp_DebugManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCFuture<void> fp_DebugManager_SubscribeToServers();
		TCFuture<TCVector<TCTrustedActor<CDebugManager>>> fp_DebugManager_GetDebugManagers(CStr _Host);

		TCFuture<uint32> fp_CommandLine_DebugManager_DebugAssetList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_DebugAssetUpload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_DebugAssetDownload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_DebugAssetDelete(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		TCFuture<uint32> fp_CommandLine_DebugManager_CrashDumpList(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_CrashDumpUpload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_CrashDumpDownload(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);
		TCFuture<uint32> fp_CommandLine_DebugManager_CrashDumpDelete(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine);

		fp64 mp_Timeout = 0.0;

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

		// Debug Manager
		TCTrustedActorSubscription<CDebugManager> mp_DebugManagers;
		CDebugManagerHelper mp_DebugManagerHelper;
	};
}
