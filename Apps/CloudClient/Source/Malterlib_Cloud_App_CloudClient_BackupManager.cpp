// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	void CCloudClientAppActor::fp_BackupManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _BackupManagerSection)
	{
		_BackupManagerSection.f_RegisterSectionOptions
			(
				{
					"BackupHost?"_=
					{
						"Names"_= {"--host"}
						, "Default"_= ""
						, "Description"_= "Limit backup query to only specified host ID"
					}
				}
			)
		;
		_BackupManagerSection.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-sources"}
					, "Description"_= "List backup sources available on remote backup managers"
				}
				, [this](CEJSON const &_Params)
				{
					return fp_CommandLine_BackupManager_ListBackupSources(_Params);
				}
			)
		;
		_BackupManagerSection.f_RegisterCommand
			(
				{
					"Names"_= {"--backup-manager-list-backups"}
					, "Description"_= "List backups available on remote backup managers"
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
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers()
	{
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");
		
		TCContinuation<void> Continuation;
		
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_SubscribeTrustedActors<NCloud::CBackupManager>
				, "MalterlibCloudBackupManager"
				, fg_ThisActor(this)
			)
			> [this, Continuation](TCAsyncResult<TCTrustedActorSubscription<CBackupManager>> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscripbe to backup managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_BackupManagers = fg_Move(*_Subscription);
				if (mp_BackupManagers.m_Actors.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any backup managers, or they are not trusted for 'MalterlibCloudBackupManager' namespace"));
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
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupHost]
			{
				TCActorResultMap<CHostInfo, CBackupManager::CListBackupSources::CResult> BackupSources;
				
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && BackupManagerInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackupSources
							, CBackupManager::CListBackupSources()
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> BackupSources.f_AddResult(BackupManagerInfo.m_HostInfo)
					;
				}
				
				BackupSources.f_GetResults() > Continuation / [this, Continuation](TCMap<CHostInfo, TCAsyncResult<CBackupManager::CListBackupSources::CResult>> &&_Results)
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
							for (auto &Source : Result->m_BackupSources)
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
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation, BackupSource, BackupHost]
			{
				TCActorResultMap<CHostInfo, CBackupManager::CListBackups::CResult> Backups;
				
				for (auto &BackupManagerInfo : mp_BackupManagers.m_Actors)
				{
					if (!BackupHost.f_IsEmpty() && BackupManagerInfo.m_HostInfo.m_HostID != BackupHost)
						continue;
					auto &BackupManager = mp_BackupManagers.m_Actors.fs_GetKey(BackupManagerInfo);
					CBackupManager::CListBackups Options;
					Options.m_ForBackupSource = BackupSource;
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackups
							, fg_Move(Options)
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> Backups.f_AddResult(BackupManagerInfo.m_HostInfo)
					;
				}
				
				Backups.f_GetResults() > Continuation / [this, Continuation](TCMap<CHostInfo, TCAsyncResult<CBackupManager::CListBackups::CResult>> &&_Results)
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
							for (auto &Backups : Result->m_Backups)
							{
								auto &BackupSouce = Result->m_Backups.fs_GetKey(Backups);
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
}
