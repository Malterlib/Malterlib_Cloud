
#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedDaemon>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Cloud/BackupManager>
#include <Mib/Cloud/FileTransfer>

#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"

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
			
			bool operator < (CBackupKey const &_Right) const
			{
				return fg_TupleReferences(m_BackupName, m_BackupTime, m_BackupID) < fg_TupleReferences(_Right.m_BackupName, _Right.m_BackupTime, _Right.m_BackupID);
			}
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
			TCLinkedList<TCFunctionMovable<void ()>> m_OnDestroyed;
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
			
			TCContinuation<void> f_Destroy();
			
			TCDistributedActor<CDirectorySyncSend> m_DirectorySyncSend;
			CActorSubscription m_Subscription;
		};
		
		struct CBackupManagerImplementation : public CBackupManager
		{
			auto f_InitBackup(CBackupKey const &_BackupKey, TCActorSubscriptionWithID<> &&_Subscription)
				-> TCContinuation<TCDistributedActorInterfaceWithID<CBackupManagerBackup>> override
			;
			
			TCContinuation<TCVector<CStr>> f_ListBackupSources() override;
			TCContinuation<TCMap<CStr, TCVector<CBackupID>>> f_ListBackups(CStr const &_ForBackupSource) override;
			auto f_DownloadBackup(CStr const &_BackupSource, CBackupID const &_BackupID, CTime const &_Time, TCActorSubscriptionWithID<> &&_Subscription)
				-> TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
				override
			;

			CBackupManagerServer *m_pThis;
		};
		
		TCContinuation<void> f_Init();
		
	private:
		TCContinuation<void> fp_Destroy() override;
		TCContinuation<void> fp_Publish();
		TCContinuation<void> fp_SetupPermissions();
		
		TCContinuation<TCVector<CStr>> fp_FilterBackupSourcesByPermissions(TCVector<CStr> const &_Sources);
		
		TCContinuation<void> fp_DestroyBackupInstance(CBackupKey const &_Key, CDistributedAppAuditor const &_Auditor, bool _bError, CStr const &_Reason);
		
		template <typename tf_CResult>
		bool fp_CheckBackupKey
			(
				CBackupManager::CBackupKey const &_BackupKey
				, CBackupKey &o_BackupKey
				, CDistributedAppAuditor const &_Auditor
				, TCContinuation<tf_CResult> &_Continuation
			)
		;		
		
		TCActor<CSeparateThreadActor> const &fp_GetQueryFileActor();
		
		TCMap<CBackupKey, CBackupInstanceManager> mp_BackupInstances;
		TCMap<CStr, CBackupDownload> mp_BackupDownloads;
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker;
		TCDelegatedActorInterface<CBackupManagerImplementation> mp_ProtocolInterface;
		CDistributedAppState &mp_AppState;
		CStr mp_FriendlyName;
		
		CTrustedPermissionSubscription mp_Permissions;
		
		TCActor<CSeparateThreadActor> mp_QueryFileActor;
	};
}
