// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
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
				, "Description"_= "Limit backup query to only specified host ID"
			}
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-sources"}
					, "Description"_= "List backup sources available on remote backup managers"
					, "Options"_=
					{
						OptionalBackupHost
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_ListBackupSources(_Params);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-backups"}
					, "Description"_= "List backups available on remote backup managers"
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
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_ListBackups(_Params);
				}
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-download-backup"}
					, "Description"_= "Download a backup from remote backup manager\n"
						"If a backup already exists the download will be resumed or ammended with the latest changes. Only appended files such as oplogs are supported\n"
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
							"By default this directory will be the 'name of the source'/'backup time and id'"
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
							, "Description"_= "Internal hidden option to forward current directory"
						}					
					}
					, "Parameters"_= 
					{
						"Backup?"_=
						{
							"Default"_= ""
							, "Description"_= "The backup to download.\n"
								"This is in the format 'Time - BackupID' as displayed in the output from --backup-manager-list-backups.\n"
						}
					}
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_DownloadBackup(_Params);
				}
			)
		;
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_BackupManager_SubscribeToServers()
	{
		if (!mp_BackupManagers.f_IsEmpty())
			return fg_Explicit();
		
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");
		
		TCContinuation<void> Continuation;
		
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CBackupManager>
				, "com.malterlib/Cloud/BackupManager"
				, fg_ThisActor(this)
			)
			> [this, Continuation](TCAsyncResult<TCTrustedActorSubscription<CBackupManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to backup managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_BackupManagers = fg_Move(*_Subscription);
				if (mp_BackupManagers.m_Actors.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any backup managers, or they are not trusted for 'com.malterlib/Cloud/BackupManager' namespace"));
					return;
				}
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}

	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		CStr BackupHost = _Params["BackupHost"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupHost]
			{
				TCActorResultMap<CHostInfo, TCVector<CStr>> BackupSources;
				
				for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = TrustedBackupManager.m_Actor;
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackupSources
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> BackupSources.f_AddResult(TrustedBackupManager.m_TrustInfo.m_HostInfo)
					;
				}
				
				BackupSources.f_GetResults() > Continuation / [Continuation](TCMap<CHostInfo, TCAsyncResult<TCVector<CStr>>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting backup sources for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Source : *Result)
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", Source));
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackups(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupSource, BackupHost]
			{
				TCActorResultMap<CHostInfo, TCMap<CStr, TCVector<CBackupManager::CBackupID>>> Backups;
				
				for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = TrustedBackupManager.m_Actor;
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackups
							, BackupSource
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> Backups.f_AddResult(TrustedBackupManager.m_TrustInfo.m_HostInfo)
					;
				}
				
				Backups.f_GetResults() > Continuation / [Continuation](TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, TCVector<CBackupManager::CBackupID>>>> &&_Results)
					{
						CDistributedAppCommandLineResults CommandLineResults;
						for (auto &Result : _Results)
						{
							auto &HostInfo = _Results.fs_GetKey(Result);
							CommandLineResults.f_AddStdOut(fg_Format("{}\n", HostInfo.f_GetDesc()));
							if (!Result)
							{
								CommandLineResults.f_AddStdErr(fg_Format("    Failed getting backups for this host: {}\n", Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Backups : *Result)
							{
								auto &BackupSouce = Result->fs_GetKey(Backups);
								CommandLineResults.f_AddStdOut(fg_Format("    {}\n", BackupSouce));
								for (auto &Backup : Backups)
									CommandLineResults.f_AddStdOut(fg_Format("        {}\n", Backup));
							}
						}
						Continuation.f_SetResult(fg_Move(CommandLineResults));
					}
				;
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_DownloadBackup(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		CStr Backup = _Params["Backup"].f_String();
		CStr Destination;
		if (auto *pValue = _Params.f_GetMember("Destination"))
			Destination = CFile::fs_GetExpandedPath(pValue->f_String(), _Params["CurrentDirectory"].f_String());
		uint64 QueueSize = _Params["BackupQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;
		
		bool bSetOwner = _Params["SetOwner"].f_Boolean();
		
		if (BackupSource.f_IsEmpty())
			return DMibErrorInstance("Backup source must be specified");
		if (Backup.f_IsEmpty())
			return DMibErrorInstance("Backup must be specified");
		
		CStr BackupID;
		CTime BackupTime;
		if (!CBackupManager::fs_IsValidBackup(Backup, &BackupID, &BackupTime))
			return DMibErrorInstance("Backup name format is invalid");
		
		if (!CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
			return DMibErrorInstance("Backup source name format is invalid");
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [=]
			{
				CStr Error;
				auto *pBackupManager = mp_BackupManagers.f_GetOneActor(BackupHost, Error);
				if (!pBackupManager)
				{
					Continuation.f_SetException
						(
							DMibErrorInstance(fg_Format("Error selecting backup manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error))
						)
					;
					return;
				}

				CStr BasePath;
				if (!Destination.f_IsEmpty())
					BasePath = Destination; 
				else if (BackupID == "Latest")
					BasePath = fg_Format("{}/{}/Latest", mp_State.m_RootDirectory, BackupSource);
				else
					BasePath = fg_Format("{}/{}/{tst.,tsb_}_{}", mp_State.m_RootDirectory, BackupSource, BackupTime, BackupID);

				CBackupManager::CBackupID DownloadBackupID;
				DownloadBackupID.m_ID = BackupID;
				DownloadBackupID.m_Time = BackupTime;
				
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
						, DownloadBackupID
						, {}
						, fg_Move(Config)
						, mp_DownloadBackupSubscription
					)
					> Continuation % "Failed to download backup" / [Continuation](CDirectorySyncReceive::CSyncResult &&_Result)
					{
						CDistributedAppCommandLineResults CommandLine;

						if (_Result.m_Stats.m_nSyncedFiles == 0)
							CommandLine.f_AddStdOut(fg_Format("All files were already up to date\n"));
						else
						{
							CommandLine.f_AddStdOut
								(
									fg_Format
									(
										"Download of {} files finished transferring: {ns } incoming bytes at {fe2} MB/s    {ns } outgoing bytes at {fe2} MB/s\n"
										, _Result.m_Stats.m_nSyncedFiles
										, _Result.m_Stats.m_IncomingBytes
										, _Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
										, _Result.m_Stats.m_OutgoingBytes
										, _Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
									)
								)
							;
						}
						Continuation.f_SetResult(fg_Move(CommandLine));
					}
				;
			}
		;
		return Continuation;
	}
}
