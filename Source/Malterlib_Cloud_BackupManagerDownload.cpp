// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_BackupManagerDownload.h"
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NPtr;
	using namespace NStr;
	using namespace NTime;
	
	TCDispatchedActorCall<CDirectorySyncReceive::CSyncResult> fg_DownloadBackup
		(
			TCDistributedActor<CBackupManager> const &_BackupManager
			, CStr const &_BackupSource
			, CBackupManager::CBackupID const &_DownloadBackupID
			, CTime const &_PointInTime
			, CDirectorySyncReceive::CConfig &&_SyncConfig
			, CActorSubscription &o_Subscription
		)
	{
		struct CState
		{
			TCActor<CDirectorySyncReceive> m_DownloadBackupReceive;
			bool m_bAborted = false;
		};
		
		TCSharedPointer<CState> pState = fg_Construct();
		
		auto ProcessingActor = fg_ConcurrentActor();
		
		o_Subscription = g_ActorSubscription(ProcessingActor) > [pState]() -> TCContinuation<void>
			{
				pState->m_bAborted = true;
				
				if (pState->m_DownloadBackupReceive)
					return pState->m_DownloadBackupReceive->f_Destroy();
				
				return fg_Explicit();
			}
		;
		
		return g_Dispatch(ProcessingActor) > [=, Config = fg_Move(_SyncConfig)]() mutable -> TCContinuation<CDirectorySyncReceive::CSyncResult>
			{
				if (pState->m_bAborted)
					return DMibErrorInstance("Aborted");
				
				TCContinuation<CDirectorySyncReceive::CSyncResult> Continuation;
				
				DMibCallActor
					(
						_BackupManager
						, CBackupManager::f_DownloadBackup
						, _BackupSource
						, _DownloadBackupID
						, _PointInTime
						, g_ActorSubscription > [pState]() -> TCContinuation<void>
						{
							if (!pState->m_DownloadBackupReceive)
								return fg_Explicit();

							return pState->m_DownloadBackupReceive->f_Destroy();
						}
					)
					> Continuation % "Failed to start download on remote server"
					/ [=, Config = fg_Move(Config)](TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_SyncClient) mutable
					{
						pState->m_DownloadBackupReceive = fg_ConstructActor<CDirectorySyncReceive>(fg_Move(Config), fg_Move(_SyncClient));
						
						pState->m_DownloadBackupReceive(&CDirectorySyncReceive::f_PerformSync)
							> Continuation % "Failed to perform backup sync"
						;
					}
				;
				return Continuation;
			}
		;
	}
}
