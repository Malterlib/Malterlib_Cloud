// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_BackupManagerDownload.h"
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud
{
	using namespace NConcurrency;
	using namespace NStorage;
	using namespace NStr;
	using namespace NTime;
	using namespace NFile;

	TCFuture<CDirectorySyncReceive::CSyncResult> DMibWorkaroundUBSanSectionErrors fg_DownloadBackup
		(
			TCDistributedActor<CBackupManager> _BackupManager
			, CStr _BackupSource
			, CTime _PointInTime
			, CDirectorySyncReceive::CConfig _SyncConfig
			, NReference::TCReference<CActorSubscription> o_Subscription
		)
	{
		co_await ECoroutineFlag_AllowReferences;

		struct CState
		{
			TCActor<CDirectorySyncReceive> m_DownloadBackupReceive;
			bool m_bAborted = false;
		};
		
		TCSharedPointer<CState> pState = fg_Construct();
		
		o_Subscription.f_Get() = g_ActorSubscription / [pState]() -> TCFuture<void>
			{
				pState->m_bAborted = true;
				if (pState->m_DownloadBackupReceive)
					co_await fg_Move(pState->m_DownloadBackupReceive).f_Destroy();
				co_return {};
			}
		;
		
		TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncClient = co_await
			(
				_BackupManager.f_CallActor(&CBackupManager::f_DownloadBackup)
				(
					CBackupManager::CDownloadBackup
					{
						_BackupSource
						, _PointInTime
						, g_ActorSubscription / [pState]() -> TCFuture<void>
						{
							if (pState->m_DownloadBackupReceive)
								co_await fg_Move(pState->m_DownloadBackupReceive).f_Destroy().f_Wrap() > fg_LogWarning("", "Failed to destroy download backup receive");

							co_return {};
						}
					}
				)
				% "Failed to start download on remote server"
			)
		;

		if (pState->m_bAborted)
			co_return DMibErrorInstance("Aborted");

		if (!SyncClient)
			co_return DMibErrorInstance("Invalid sync client");

		pState->m_DownloadBackupReceive = fg_ConstructActor<CDirectorySyncReceive>(fg_Move(_SyncConfig), fg_Move(SyncClient));

		CDirectorySyncReceive::CSyncResult Result = co_await (pState->m_DownloadBackupReceive(&CDirectorySyncReceive::f_PerformSync) % "Failed to perform backup sync");

		if (pState->m_DownloadBackupReceive)
			co_await fg_Move(pState->m_DownloadBackupReceive).f_Destroy();

		co_return fg_Move(Result);
	}
}
