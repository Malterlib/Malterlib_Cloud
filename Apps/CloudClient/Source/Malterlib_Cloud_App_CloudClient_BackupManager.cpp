// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/BackupManagerDownload>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto OptionalBackupHost = "BackupHost?"_=
			{
				"Names"_= {"--host"}
				, "Default"_= ""
				, "Description"_= "Limit backup query to only specified host ID."
			}
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-sources"}
					, "Description"_= "List backup sources available on remote backup managers."
					, "Options"_=
					{
						OptionalBackupHost
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_BackupManager_ListBackupSources(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-backups"}
					, "Description"_= "List backups available on remote backup managers."
					, "Options"_=
					{
						OptionalBackupHost
					}
					, "Parameters"_= 
					{
						"BackupSource?"_=
						{
							"Default"_= ""
							, "Description"_= "The backup source to list backups for.\n"
								"If left empty backups will be listed for all sources you have access to.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_BackupManager_ListBackups(_Params, _pCommandLine);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-download-backup"}
					, "Description"_= "Download a backup from remote backup manager.\n"
						"If a backup already exists the download will be resumed or ammended with the latest changes. Only appended files such as oplogs are supported.\n"
					, "Options"_=
					{
						"BackupHost?"_=
						{
							"Names"_= {"--host"}
							, "Default"_= ""
							, "Description"_= "The host ID of the host to download the backup from."
						}					
						, "BackupSource"_=
						{
							"Names"_= {"--source"}
							, "Type"_= ""
							, "Description"_= "The backup source to download from."
						}
						, "BackupQueueSize?"_=
						{
							"Names"_= {"--queue-size"}
							, "Default"_= int64(8*1024*1024)
							, "Description"_= "The amount of data to keep in flight while downloading."
						}
						, "Destination?"_=
						{
							"Names"_= {"--destination"}
							, "Type"_= ""
							, "Description"_= "The directory to download to.\n"
							"By default this directory will be the 'name of the source'/'backup time'"
						}
						, "SetOwner?"_=
						{
							"Names"_= {"--set-owner"}
							, "Default"_= false
							, "Description"_= "Set owner and group on the files downloaded.\n"
						}
						, "CurrentDirectory?"_=
						{
							"Names"_= _[_]
							, "Default"_= CFile::fs_GetCurrentDirectory()
							, "Hidden"_= true
							, "Description"_= "Internal hidden option to forward current directory."
						}					
					}
					, "Parameters"_= 
					{
						"BackupTime?"_=
						{
							"Default"_= NTime::CTime{}
							, "Description"_= "The time of the backup to download.\n"
								"Leave as default to download the latest backup.\n"
						}
					}
				}
				, [this](CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
				{
					return fp_CommandLine_BackupManager_DownloadBackup(_Params, _pCommandLine);
				}
			)
		;
	}
	
	TCFuture<void> CCloudClientAppActor::fp_BackupManager_SubscribeToServers()
	{
		if (!mp_BackupManagers.f_IsEmpty())
			return fg_Explicit();
		
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");
		
		TCPromise<void> Promise;
		
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CBackupManager>
				, "com.malterlib/Cloud/BackupManager"
				, fg_ThisActor(this)
			)
			> [this, Promise](TCAsyncResult<TCTrustedActorSubscription<CBackupManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to backup managers: {}", _Subscription.f_GetExceptionStr());
					Promise.f_SetException(_Subscription);
					return;
				}
				mp_BackupManagers = fg_Move(*_Subscription);
				if (mp_BackupManagers.m_Actors.f_IsEmpty())
				{
					Promise.f_SetException(DMibErrorInstance("Not connected to any backup managers, or they are not trusted for 'com.malterlib/Cloud/BackupManager' namespace"));
					return;
				}
				Promise.f_SetResult();
			}
		;
		return Promise.f_MoveFuture();
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCPromise<uint32> Promise;
		CStr BackupHost = _Params["BackupHost"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Promise / [=]
			{
				TCActorResultMap<CHostInfo, TCVector<CStr>> BackupSources;
				
				for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = TrustedBackupManager.m_Actor;
					BackupManager.f_CallActor(&CBackupManager::f_ListBackupSources)()
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> BackupSources.f_AddResult(TrustedBackupManager.m_TrustInfo.m_HostInfo)
					;
				}
				
				BackupSources.f_GetResults() > Promise / [=](TCMap<CHostInfo, TCAsyncResult<TCVector<CStr>>> &&_Results)
					{
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							*_pCommandLine += "{}\n"_f << HostInfo.f_GetDesc();
							if (!Result)
							{
								*_pCommandLine += "    Failed getting backup sources for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}
							for (auto &Source : *Result)
								*_pCommandLine += "    {}\n"_f << Source;
						}
						Promise.f_SetResult(0);
					}
				;
			}
		;
		return Promise.f_MoveFuture();
	}
	
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCPromise<uint32> Promise;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Promise / [=]
			{
				TCActorResultMap<CHostInfo, TCMap<CStr, CBackupManager::CBackupInfo>> Backups;
				
				for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = TrustedBackupManager.m_Actor;
					BackupManager.f_CallActor(&CBackupManager::f_ListBackups)(BackupSource)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> Backups.f_AddResult(TrustedBackupManager.m_TrustInfo.m_HostInfo)
					;
				}
				
				Backups.f_GetResults() > Promise / [=](TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CBackupManager::CBackupInfo>>> &&_Results)
					{
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							*_pCommandLine += "{}\n"_f << HostInfo.f_GetDesc();
							if (!Result)
							{
								*_pCommandLine %= "    Failed getting backups for this host: {}\n"_f << Result.f_GetExceptionStr();
								continue;
							}
							for (auto &BackupInfo : *Result)
							{
								auto &BackupSouce = Result->fs_GetKey(BackupInfo);
								*_pCommandLine += "    {}   {} -> {}\n"_f << BackupSouce << BackupInfo.m_Earliest << BackupInfo.m_Latest;
								for (auto &Snapshot : BackupInfo.m_Snapshots)
									*_pCommandLine += "        {}\n"_f << Snapshot;
							}
						}
						Promise.f_SetResult(0);
					}
				;
			}
		;
		return Promise.f_MoveFuture();
	}
	
	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params, NStorage::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		TCPromise<uint32> Promise;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		NTime::CTime BackupTime = _Params["BackupTime"].f_Date();
		CStr Destination;
		if (auto *pValue = _Params.f_GetMember("Destination"))
			Destination = CFile::fs_GetExpandedPath(pValue->f_String(), _Params["CurrentDirectory"].f_String());
		uint64 QueueSize = _Params["BackupQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;
		
		bool bSetOwner = _Params["SetOwner"].f_Boolean();
		
		if (BackupSource.f_IsEmpty())
			return DMibErrorInstance("Backup source must be specified");

		if (!CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
			return DMibErrorInstance("Backup source name format is invalid");
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Promise / [=]
			{
				CStr Error;
				auto *pBackupManager = mp_BackupManagers.f_GetOneActor(BackupHost, Error);
				if (!pBackupManager)
				{
					Promise.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting backup manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				CStr BasePath;
				if (!Destination.f_IsEmpty())
					BasePath = Destination; 
				else if (BackupTime.f_IsValid())
					BasePath = fg_Format("{}/{}/{tst.,tsb_}", mp_State.m_RootDirectory, BackupSource, BackupTime);
				else
					BasePath = fg_Format("{}/{}/Latest", mp_State.m_RootDirectory, BackupSource);

				CDirectorySyncReceive::CConfig Config;
				Config.m_BasePath = BasePath;
				Config.m_PreviousBasePath = BasePath;
				Config.m_QueueSize = QueueSize;
				Config.m_PreviousManifest = BasePath + ".manifest";
				Config.m_OutputManifestPath = BasePath + ".manifest";
				Config.m_ExcessFilesAction = CDirectorySyncReceive::EExcessFilesAction_Ignore;
				if (!bSetOwner)
					Config.m_SyncFlags = CDirectorySyncReceive::ESyncFlag_WriteTime | CDirectorySyncReceive::ESyncFlag_Attributes;
				
				fg_DownloadBackup
					(
						pBackupManager->m_Actor
						, BackupSource
						, BackupTime
						, fg_Move(Config)
						, mp_DownloadBackupSubscription
					)
					> Promise % "Failed to download backup" / [=](CDirectorySyncReceive::CSyncResult &&_Result)
					{
						if (_Result.m_Stats.m_nSyncedFiles == 0)
							*_pCommandLine += "All files were already up to date\n";
						else
						{
							*_pCommandLine += "Download of {} files finished transferring: {ns } incoming bytes at {fe2} MB/s    {ns } outgoing bytes at {fe2} MB/s\n"_f
								<< _Result.m_Stats.m_nSyncedFiles
								<< _Result.m_Stats.m_IncomingBytes
								<< _Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
								<< _Result.m_Stats.m_OutgoingBytes
								<< _Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
							;
						}
						Promise.f_SetResult(0);
					}
				;
			}
		;
		return Promise.f_MoveFuture();
	}
}
