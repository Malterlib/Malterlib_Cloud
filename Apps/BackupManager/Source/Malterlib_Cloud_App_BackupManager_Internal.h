
#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/FileTransfer>

#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupSource.h"

namespace NMib::NCloud::NBackupManager
{
	struct CBackupManagerServer : public CActor
	{
	public:
		using CActorHolder = CDelegatedActorHolder;

		CBackupManagerServer(CDistributedAppState &_AppState);
		~CBackupManagerServer();

		struct CBackupKey
		{
			CStr m_BackupName;
			CTime m_BackupTime;
			CStr m_BackupID;

			CStr f_GetDesc() const;

			auto operator <=> (CBackupKey const &_Right) const = default;
		};

		struct CTransferStats
		{
			uint64 m_nTransferredBytes = 0;
			CClock m_Clock{true};
		};

		struct CBackupInstanceManager;
		struct CBackupInstanceManager
		{
			CBackupKey const &f_GetKey() const
			{
				return TCMap<CBackupKey, CBackupInstanceManager>::fs_GetKey(*this);
			}

			TCDistributedActor<CBackupInstance> m_BackupInstance;
			TCLinkedList<TCPromise<void>> m_OnDestroyed;
			CDistributedAppAuditor m_OwningHost;
			CActorSubscription m_OnDisconnectSubscrption;
			CActorSubscription m_BackupRunningSubscription;

			bool m_bPendingDestroy = false;
		};

		struct CBackupDownload
		{
			CBackupDownload();
			~CBackupDownload();

			CStr const &f_GetDownloadID() const
			{
				return TCMap<CStr, CBackupDownload>::fs_GetKey(*this);
			}

			TCFuture<void> f_Destroy();

			TCDistributedActor<CDirectorySyncSend> m_DirectorySyncSend;
			CActorSubscription m_Subscription;
		};

		struct CBackupManagerImplementation : public CBackupManager
		{
			auto f_InitBackup(CBackupManager::CInitBackup &&_Params)
				-> TCFuture<TCDistributedActorInterfaceWithID<CBackupManagerBackup>> override
			;

			TCFuture<TCVector<CStr>> f_ListBackupSources() override;
			TCFuture<TCMap<CStr, CBackupInfo>> f_ListBackups(CStr const &_ForBackupSource) override;
			auto f_DownloadBackup(CDownloadBackup &&_DownloadBackup)
				-> TCFuture<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
				override
			;

			DMibDelegatedActorImplementation(CBackupManagerServer);
		};

		TCFuture<void> f_Init();

	private:
		TCFuture<void> fp_Destroy() override;
		TCFuture<void> fp_Publish();
		TCFuture<void> fp_SetupPermissions();
		TCActor<CBackupSource> const &fp_CreateBackupSource(CStr const &_Source);
		TCActor<CBackupSource> const *fp_GetBackupSource(CStr const &_Source);

		TCFuture<TCVector<CStr>> fp_EnumBackupSourcesFromDisk();
		TCVector<CStr> fp_EnumBackupSources();
		TCFuture<TCVector<CStr>> fp_FilterBackupSourcesByPermissions(TCVector<CStr> const &_Sources);

		TCFuture<void> fp_DestroyBackupInstance(CBackupKey const &_Key, CDistributedAppAuditor const &_Auditor, bool _bError, CStr const &_Reason);

		CExceptionPointer fp_CheckBackupKey
			(
				CBackupManager::CBackupKey const &_BackupKey
				, CBackupKey &o_BackupKey
				, CDistributedAppAuditor const &_Auditor
			)
		;

		TCMap<CStr, TCActor<CBackupSource>> mp_BackupSources;
		TCMap<CBackupKey, CBackupInstanceManager> mp_BackupInstances;
		TCMap<CStr, CBackupDownload> mp_BackupDownloads;
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;
		TCDistributedActorInstance<CBackupManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;
		CStr mp_FriendlyName;

		CTrustedPermissionSubscription mp_Permissions;
	};
}
