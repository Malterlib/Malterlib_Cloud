// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/BackupManagerDownload>
#include <Mib/CommandLine/TableRenderer>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		auto OptionalBackupHost = "BackupHost?"_o=
			{
				"Names"_o= {"--host"}
				, "Default"_o= ""
				, "Description"_o= "Limit backup query to only specified host ID."
			}
		;
		auto IncludeHost = "IncludeHost?"_o=
			{
				"Names"_o= {"--include-host"}
				, "Default"_o= false
				, "Description"_o= "Include version manager host in output.\n"
			}
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--backup-manager-list-sources"}
					, "Description"_o= "List backup sources available on remote backup managers."
					, "Options"_o=
					{
						OptionalBackupHost
						, IncludeHost
						, CTableRenderHelper::fs_OutputTypeOption()
					}
				}
				, [this](CEJSONSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_BackupManager_ListBackupSources(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--backup-manager-list-backups"}
					, "Description"_o= "List backups available on remote backup managers."
					, "Options"_o=
					{
						OptionalBackupHost
						, IncludeHost
						, CTableRenderHelper::fs_OutputTypeOption()
					}
					, "Parameters"_o=
					{
						"BackupSource?"_o=
						{
							"Default"_o= ""
							, "Description"_o= "The backup source to list backups for.\n"
								"If left empty backups will be listed for all sources you have access to.\n"
						}
					}
				}
				, [this](CEJSONSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_BackupManager_ListBackups(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= {"--backup-manager-download-backup"}
					, "Description"_o= "Download a backup from remote backup manager.\n"
						"If a backup already exists the download will be resumed or ammended with the latest changes. Only appended files such as oplogs are supported.\n"
					, "Options"_o=
					{
						"BackupHost?"_o=
						{
							"Names"_o= {"--host"}
							, "Default"_o= ""
							, "Description"_o= "The host ID of the host to download the backup from."
						}
						, "BackupSource"_o=
						{
							"Names"_o= {"--source"}
							, "Type"_o= ""
							, "Description"_o= "The backup source to download from."
						}
						, "BackupQueueSize?"_o=
						{
							"Names"_o= {"--queue-size"}
							, "Default"_o= int64(NFile::gc_IdealNetworkQueueSize)
							, "Description"_o= "The amount of data to keep in flight while downloading."
						}
						, "Destination?"_o=
						{
							"Names"_o= {"--destination"}
							, "Type"_o= ""
							, "Description"_o= "The directory to download to.\n"
							"By default this directory will be the 'name of the source'/'backup time'."
						}
						, "SetOwner?"_o=
						{
							"Names"_o= {"--set-owner"}
							, "Default"_o= false
							, "Description"_o= "Set owner and group on the files downloaded.\n"
						}
						, "FindClosestSnapshot?"_o=
						{
							"Names"_o= {"--find-closest-snapshot"}
							, "Default"_o= false
							, "Description"_o= "Find the closest snapshot before the specified backup time.\n"
						}
						, "CurrentDirectory?"_o=
						{
							"Names"_o= _[_]
							, "Default"_o= CFile::fs_GetCurrentDirectory()
							, "Hidden"_o= true
							, "Description"_o= "Internal hidden option to forward current directory."
						}
					}
					, "Parameters"_o=
					{
						"BackupTime?"_o=
						{
							"Default"_o= NTime::CTime{}
							, "Description"_o= "The time of the backup to download.\n"
								"Leave as default to download the latest backup.\n"
						}
					}
				}
				, [this](CEJSONSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_BackupManager_DownloadBackup(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_BackupManager_SubscribeToServers()
	{
		if (!mp_BackupManagers.f_IsEmpty())
			co_return {};

		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");

		auto Subscription = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<NCloud::CBackupManager>().f_Wrap();

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to backup managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_BackupManagers = fg_Move(*Subscription);
		if (mp_BackupManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any backup managers, or they are not trusted for 'com.malterlib/Cloud/BackupManager' namespace");

		co_return {};
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackupSources(CEJSONSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr BackupHost = _Params["BackupHost"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await fp_BackupManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers");

		TCFutureMap<CHostInfo, TCVector<CStr>> BackupSources;

		for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
		{
			if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
				continue;

			auto &BackupManager = TrustedBackupManager.m_Actor;
			BackupManager.f_CallActor(&CBackupManager::f_ListBackupSources)()
				.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
				> BackupSources[TrustedBackupManager.m_TrustInfo.m_HostInfo]
			;
		}

		TCMap<CHostInfo, TCAsyncResult<TCVector<CStr>>> Results = co_await fg_AllDoneWrapped(BackupSources);

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Host", "Source");

		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting backup sources for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}
			for (auto &Source : *Result)
				TableRenderer.f_AddRow(HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags), Source);
		}

		if (!bIncludeHost)
			TableRenderer.f_RemoveColumn(0);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackups(CEJSONSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		bool bIncludeHost = _Params["IncludeHost"].f_Boolean();

		co_await fp_BackupManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers");
		TCFutureMap<CHostInfo, TCMap<CStr, CBackupManager::CBackupInfo>> Backups;

		for (auto &TrustedBackupManager : mp_BackupManagers.m_Actors)
		{
			if (!BackupHost.f_IsEmpty() && TrustedBackupManager.m_TrustInfo.m_HostInfo.m_HostID != BackupHost)
				continue;
			auto &BackupManager = TrustedBackupManager.m_Actor;
			BackupManager.f_CallActor(&CBackupManager::f_ListBackups)(BackupSource)
				.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
				> Backups[TrustedBackupManager.m_TrustInfo.m_HostInfo]
			;
		}

		TCMap<CHostInfo, TCAsyncResult<TCMap<CStr, CBackupManager::CBackupInfo>>> Results = co_await fg_AllDoneWrapped(Backups);

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		CTableRenderHelper TableRenderer = _pCommandLine->f_TableRenderer();
		TableRenderer.f_AddHeadings("Host", "Source", "Earliest", "Latest", "Snapshots");

		for (auto &Result : Results)
		{
			auto &HostInfo = Results.fs_GetKey(Result);
			if (!Result)
			{
				*_pCommandLine %= "{}Failed getting backups for host{} '{}': {}\n"_f
					<< AnsiEncoding.f_StatusError()
					<< AnsiEncoding.f_Default()
					<< HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
					<< Result.f_GetExceptionStr()
				;
				continue;
			}
			for (auto &BackupInfo : *Result)
			{
				auto &BackupSource = Result->fs_GetKey(BackupInfo);
				TCVector<CStr> Snapshots;
				for (auto &Snapshot : BackupInfo.m_Snapshots)
					Snapshots.f_Insert("{}\n"_f << Snapshot);

				TableRenderer.f_AddRow
					(
						HostInfo.f_GetDescColored(_pCommandLine->m_AnsiFlags)
						, BackupSource
						, "{}"_f << BackupInfo.m_Earliest
						, "{}"_f << BackupInfo.m_Latest
						, CStr::fs_Join(Snapshots, "\n")
					)
				;
			}
		}

		if (!bIncludeHost)
			TableRenderer.f_RemoveColumn(0);

		TableRenderer.f_Output(_Params);

		co_return 0;
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_BackupManager_DownloadBackup(CEJSONSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr BackupHost = _Params["BackupHost"].f_String();
		CStr BackupSource = _Params["BackupSource"].f_String();
		CTime BackupTime = _Params["BackupTime"].f_Date();
		CStr Destination;
		if (auto *pValue = _Params.f_GetMember("Destination"))
			Destination = CFile::fs_GetExpandedPath(pValue->f_String(), _Params["CurrentDirectory"].f_String());
		uint64 QueueSize = _Params["BackupQueueSize"].f_Integer();
		if (QueueSize < 128*1024)
			QueueSize = 128*1024;

		bool bSetOwner = _Params["SetOwner"].f_Boolean();
		bool bFindClosestSnapshot = _Params["FindClosestSnapshot"].f_Boolean();

		if (BackupSource.f_IsEmpty())
			co_return DMibErrorInstance("Backup source must be specified");

		if (!CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
			co_return DMibErrorInstance("Backup source name format is invalid");

		co_await fp_BackupManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers");

		CStr Error;
		auto *pBackupManager = mp_BackupManagers.f_GetOneActor(BackupHost, Error);
		if (!pBackupManager)
			co_return DMibErrorInstance(fg_Format("Error selecting backup manager: {}. Connection might have failed. Use --log-to-stderr to see more info.", Error));

		if (bFindClosestSnapshot && BackupTime.f_IsValid())
		{
			auto Backups = co_await (pBackupManager->m_Actor.f_CallActor(&CBackupManager::f_ListBackups)(BackupSource) % "Failed to list backups");

			CTime BestMatch;
			for (auto &BackupInfo : Backups)
			{
				for (auto &SnapshotTime : BackupInfo.m_Snapshots)
				{
					if (SnapshotTime <= BackupTime && (!BestMatch.f_IsValid() || SnapshotTime > BestMatch))
						BestMatch = SnapshotTime;
				}
			}

			if (!BestMatch.f_IsValid())
				co_return DMibErrorInstance("Cloud not find a matching backup that was before the specified time: {} UTC"_f << BackupTime);

			BackupTime = BestMatch;
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

		CDirectorySyncReceive::CSyncResult Result = co_await
			(
				fg_DownloadBackup
				(
					pBackupManager->m_Actor
					, BackupSource
					, BackupTime
					, fg_Move(Config)
					, fg_Reference(mp_DownloadBackupSubscription)
				)
				% "Failed to download backup"
			)
		;

		if (Result.m_Stats.m_nSyncedFiles == 0)
			*_pCommandLine += "All files were already up to date\n";
		else
		{
			*_pCommandLine += "Download of {} files finished transferring: {ns } incoming bytes at {fe2} MB/s    {ns } outgoing bytes at {fe2} MB/s\n"_f
				<< Result.m_Stats.m_nSyncedFiles
				<< Result.m_Stats.m_IncomingBytes
				<< Result.m_Stats.f_IncomingBytesPerSecond() / 1'000'000.0
				<< Result.m_Stats.m_OutgoingBytes
				<< Result.m_Stats.f_OutgoingBytesPerSecond() / 1'000'000.0
			;
		}

		co_return 0;
	}
}
