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
	}
	
	TCContinuation<void> CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers()
	{
		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to backup managers");
		
		TCContinuation<void> Continuation;
		
		mp_State.m_DistributionManager
			(
				&CActorDistributionManager::f_SubscribeActors
				, fg_CreateVector<CStr>("MalterlibCloudBackupManager")
				, fg_ThisActor(this)
				, [this](CAbstractDistributedActor &&_NewActor)
				{
					CStr HostID = _NewActor.f_GetRealHostID();
					
					auto Manager = _NewActor.f_GetActor<NCloud::CBackupManager>();
					mp_BackupManagerToHost[Manager] = HostID;
					mp_HostToBackupManager[HostID] = Manager;
				}
				, [this](TCWeakDistributedActor<CActor> const &_RemovedActor)
				{
					if (auto *pHostID = mp_BackupManagerToHost.f_FindEqual(_RemovedActor))
					{
						mp_HostToBackupManager.f_Remove(*pHostID);
						mp_BackupManagerToHost.f_Remove(pHostID);
					}
				}
			)
			> [this, Continuation](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				if (!_Subscription)
				{
					DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscripbe to backup managers: {}", _Subscription.f_GetExceptionStr());
					Continuation.f_SetException(_Subscription);
					return;
				}
				mp_BackupServerActorsSubscription = fg_Move(*_Subscription);
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}
	
	TCContinuation<CDistributedAppCommandLineResults> CCloudClientAppActor::fp_CommandLine_BackupManager_ListBackupSources(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		fg_ThisActor(this)(&CCloudClientAppActor::fp_BackupManager_SubscribeToBackupServers).f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for backup servers") 
			> Continuation / [this, Continuation]
			{
				TCActorResultMap<CStr, CBackupManager::CListBackupSources::CResult> BackupSources;
				
				if (mp_HostToBackupManager.f_IsEmpty())
				{
					Continuation.f_SetException(DMibErrorInstance("Not connected to any backup managers"));
					return;
				}
				
				for (auto &BackupManager : mp_HostToBackupManager)
				{
					CStr HostID = mp_HostToBackupManager.fs_GetKey(BackupManager);
					DMibCallActor
						(
							BackupManager
							, CBackupManager::f_ListBackupSources
							, CBackupManager::CListBackupSources()
						)
						.f_Timeout(mp_Timeout, "Timed out waiting for backup manager to reply")
						> BackupSources.f_AddResult(mp_HostToBackupManager.fs_GetKey(BackupManager))
					;
				}
				
				BackupSources.f_GetResults() > Continuation / [this, Continuation](TCMap<CStr, TCAsyncResult<CBackupManager::CListBackupSources::CResult>> &&_Results)
					{
						CDistributedAppCommandLineResults Resuts;
						for (auto &Result : _Results)
						{
							if (!Result)
							{
								Resuts.f_AddStdErr(fg_Format("Failed getting backup sources for host '{}': {}\n", _Results.fs_GetKey(Result), Result.f_GetExceptionStr()));
								continue;
							}
							for (auto &Source : Result->m_BackupSources)
								Resuts.f_AddStdOut(fg_Format("{}\n", Source));
						}
						Continuation.f_SetResult(fg_Move(Resuts));
					}
				;
			}
		;
		
		return Continuation;
	}
}
