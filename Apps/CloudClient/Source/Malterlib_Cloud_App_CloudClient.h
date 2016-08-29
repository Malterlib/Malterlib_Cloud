// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedApp>
#include <Mib/Daemon/Daemon>
#include <Mib/Cloud/BackupManager>

namespace NMib::NCloud::NCloudClient
{
	struct CCloudClientAppActor : public CDistributedAppActor
	{
		CCloudClientAppActor();
		~CCloudClientAppActor();
		
	private:
		TCContinuation<void> fp_StartApp(NEncoding::CEJSON const &_Params) override;
		TCContinuation<void> fp_StopApp() override;
		void fp_BuildCommandLine(CDistributedAppCommandLineSpecification &o_CommandLine) override;
		
		TCContinuation<void> fp_Initialize();
		
		void fp_ParseCommonOptions(NEncoding::CEJSON const &_Params);
		
		// Backup Manager
		void fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _BackupManagerSection);
		TCContinuation<void> fp_BackupManager_SubscribeToBackupServers();
		TCContinuation<CDistributedAppCommandLineResults> fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params);
		
		fp64 mp_Timeout = 0.0;
		
		// Backup Manager
		CActorSubscription mp_BackupServerActorsSubscription;
		TCMap<TCDistributedActor<CBackupManager>, CStr> mp_BackupManagerToHost;
		TCMap<CStr, TCDistributedActor<CBackupManager>> mp_HostToBackupManager;
	};
}
