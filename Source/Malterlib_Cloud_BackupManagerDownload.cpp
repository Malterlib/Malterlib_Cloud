// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_BackupManagerDownload.h"
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;
	
	TCDispatchedActorCall<NFile::CDirectorySyncReceive::CSyncResult> fg_DownloadBackup
		(
			TCDistributedActor<CBackupManager> const &_BackupManager
			, CStr const &_BackupSource
			, CTime const &_PointInTime
			, NFile::CDirectorySyncReceive::CConfig &&_SyncConfig
			, CActorSubscription &o_Subscription
		)
	{
		struct CState
		{
			TCActor<NFile::CDirectorySyncReceive> m_DownloadBackupReceive;
			bool m_bAborted = false;
		};
		
		TCSharedPointer<CState> pState = fg_Construct();
		
		auto ProcessingActor = fg_ConcurrentActor();
		
		o_Subscription = g_ActorSubscription(ProcessingActor) / [pState]() -> TCContinuation<void>
			{
				pState->m_bAborted = true;
				
				if (!pState->m_DownloadBackupReceive)
					return fg_Explicit();

				auto DownloadBackupReceive = fg_Move(pState->m_DownloadBackupReceive);
				return DownloadBackupReceive->f_Destroy();
			}
		;
		
		return g_Dispatch(ProcessingActor) / [=, Config = fg_Move(_SyncConfig)]() mutable -> TCContinuation<NFile::CDirectorySyncReceive::CSyncResult>
			{
				if (pState->m_bAborted)
					return DMibErrorInstance("Aborted");
				
				TCContinuation<NFile::CDirectorySyncReceive::CSyncResult> Continuation;
				
				DMibCallActor
					(
						_BackupManager
						, CBackupManager::f_DownloadBackup
					 	, CBackupManager::CDownloadBackup
						{
							_BackupSource
							, _PointInTime
							, g_ActorSubscription / [pState]() -> TCContinuation<void>
							{
								if (!pState->m_DownloadBackupReceive)
									return fg_Explicit();

								auto DownloadBackupReceive = fg_Move(pState->m_DownloadBackupReceive);
								return DownloadBackupReceive->f_Destroy();
							}
						}
					)
					> Continuation % "Failed to start download on remote server"
					/ [=, Config = fg_Move(Config)](TCDistributedActorInterfaceWithID<NFile::CDirectorySyncClient> &&_SyncClient) mutable
					{
						pState->m_DownloadBackupReceive = fg_ConstructActor<NFile::CDirectorySyncReceive>(fg_Move(Config), fg_Move(_SyncClient));
						
						pState->m_DownloadBackupReceive(&NFile::CDirectorySyncReceive::f_PerformSync)
							> Continuation % "Failed to perform backup sync" / [=](NFile::CDirectorySyncReceive::CSyncResult &&_Result)
							{
								if (!pState->m_DownloadBackupReceive)
									return Continuation.f_SetResult(fg_Move(_Result));

								auto DownloadBackupReceive = fg_Move(pState->m_DownloadBackupReceive);
								DownloadBackupReceive->f_Destroy() > Continuation / [=, Result = fg_Move(_Result)]() mutable
									{
										Continuation.f_SetResult(fg_Move(Result));
									}
								;
							}
						;
					}
				;
				return Continuation;
			}
		;
	}
}
