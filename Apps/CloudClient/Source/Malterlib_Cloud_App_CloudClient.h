// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/VersionManager>

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppActor : public CDistributedAppActor
	{
		CCloudClientAppActor();
		~CCloudClientAppActor();
		
	private:
		
		struct CSelfUpdateVersion
		{
			TCDistributedActor<CVersionManager> m_Actor;
			CVersionManager::CVersionID m_VersionID;
		};
		
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		TCContinuation<CDistributedAppCommandLineResults> fp_PreRunCommandLine(CStr const &_Command, NEncoding::CEJSON const &_Params) override;
		
		TCContinuation<CSelfUpdateVersion> fp_GetSelfUpdateVersion();
		
		TCContinuation<void> fp_Initialize();

		void fp_ParseCommonOptions(NEncoding::CEJSON const &_Params);

		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_SelfUpdate(CEJSON const &_Params);
		
		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCContinuation<void> fp_BackupManager_SubscribeToServers();
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params);

		// Version Manager
		void fp_VersionManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section);
		TCContinuation<void> fp_VersionManager_SubscribeToServers();
		
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_VersionManager_ListApplications(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_VersionManager_ListVersions(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_VersionManager_UploadVersion(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_VersionManager_DownloadVersion(CEJSON const &_Params);
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_VersionManager_ChangeTags(CEJSON const &_Params);
		
		fp64 mp_Timeout = 0.0;
		
		// Backup Manager
		TCTrustedActorSubscription<CBackupManager> mp_BackupManagers;
		CActorSubscription mp_DownloadBackupSubscription;

		// Version Manager
		TCTrustedActorSubscription<CVersionManager> mp_VersionManagers;
		CVersionManagerHelper mp_VersionManagerHelper;
	};
}
