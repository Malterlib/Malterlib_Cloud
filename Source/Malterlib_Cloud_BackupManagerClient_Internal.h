// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"

#include <Mib/Cloud/BackupManager>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Cryptography/Hashes/SHA>

using namespace NMib; 
using namespace NMib::NConcurrency; 
using namespace NMib::NContainer; 
using namespace NMib::NPtr; 
using namespace NMib::NFile; 
using namespace NMib::NStr; 
using namespace NMib::NDataProcessing;

namespace NMib::NCloud
{
	struct CBackupManagerClient::CInternal
	{
		CInternal
			(
				CBackupManagerClient *_pThis
				, CConfig const &_Config
				, TCActor<CDistributedActorTrustManager> const &_TrustManager
				, TCActorFunctor
				<
					TCContinuation<TCActorSubscriptionWithID<>>
					(
						TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> &&_BackupInterface
						, CActorSubscription &&_ManifestFinished
						, CStr const &_BackupRoot
					)
				>
				&&_fOnNewBackup
			)
		;
		~CInternal();

		struct CWatchedPath
		{
			CStr const &f_GetPath() const
			{
				return TCMap<CStr, CWatchedPath>::fs_GetKey(*this);
			}
			
			CActorSubscription m_Subscription;
			bool m_bRecursive = false;
			bool m_bPending = false;
		};
		
		struct CUpdatedDirectory
		{
			CDirectoryManifestFile m_ManifestFile;
			bool m_bAdded = false;
		};
		
		struct CUpdateManifestResult
		{
			CDirectoryManifestFile m_ManifestFile;
			TCMap<CStr, CUpdatedDirectory> m_UpdatedDirectories;
			CUniqueFileIdentifier m_FileID;
			bool m_bExists = false;
			bool m_bRemoved = false;
			bool m_bAdded = false;
			bool m_bIDChanged = false;
			bool m_Appended = false;
		};
		
		struct CAppendFileState
		{
			CFile m_File;
			CHash_SHA256 m_Hash;
			CUniqueFileIdentifier m_FileID;
			CDirectoryManifestFile m_ManifestFile;
			bool m_bIsLink = false;
		};

		struct CNotifacitonSubscription
		{
			CBackupManagerClient::ENotification m_Notifications;
			TCActorFunctor<TCContinuation<void> (CHostInfo const &_RemoteHost, CBackupManagerClient::CNotification &&_Notification)> m_fOnNotification;
		};
		
		struct CDistributedAppInterfaceBackupImplementation : public CDistributedAppInterfaceBackup
		{
			TCContinuation<void> f_AppendManifest(NFile::CDirectoryManifestConfig const &_Config) override;
			TCContinuation<TCActorSubscriptionWithID<>> f_SubscribeInitialFinished(TCActorFunctorWithID<TCContinuation<void> ()> &&_fOnInitialFinished) override;
			TCContinuation<TCActorSubscriptionWithID<>> f_SubscribeBackupStopped(TCActorFunctorWithID<TCContinuation<void> ()> &&_fOnStopped) override;
			
			CBackupManagerClient *m_pThis = nullptr;
		};
		
		void f_Construct(TCActor<CActorDistributionManager> const &_DistributionManager);
		void f_NewBackupKey();
		void f_RunBackup();
		void f_Subscribe();
		void f_BackupFinishedStarting();
		TCContinuation<void> f_SubscribeChanges();
		TCContinuation<void> f_RetrySubscribeChanges();
		void f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification);
		bool f_IsPathInManifest(CStr const &_Path, CStr &o_FileName);
		static void fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed);
		
		TCContinuation<CUpdateManifestResult> f_UpdateManifest(CStr const &_FileName, CStr const &_OriginalFileName, bool _bDirtyHint);
		
		CBackupManagerClient *m_pThis = nullptr;
		TCSharedPointer<NAtomic::TCAtomic<bool>> m_pDestroyed;
		CConfig m_Config;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		
		TCActor<CSeparateThreadActor> m_FileActor;
		CDirectoryManifest m_Manifest; // Kept up to date
		TCMap<CStr, CUniqueFileIdentifier> m_ManifestFileIDs;
		TCMap<CStr, TCSharedPointer<CAppendFileState>> m_AppendStates;

		TCActor<CFileChangeNotificationActor> m_FileChangeNotificationsActor;
		TCMap<CStr, CWatchedPath> m_WatchedPaths;
		TCMap<CStr, CWatchedPath> m_WatchedPathsMissing;
		
		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotifications;
		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotificationsMissing;
		
		TCTrustedActorSubscription<CBackupManager> m_BackupManagers;
		
		TCMap<TCWeakActor<CBackupManager>, TCActor<NPrivate::CBackupManagerClient_Instance>> m_RunningBackupInstances;
		
		TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker = fg_Construct();
		
		CBackupManager::CBackupKey m_BackupKey;
		
		TCMap<CStr, CNotifacitonSubscription> m_NotificationSubscriptions;
		TCMap<CStr, TCActorFunctorWithID<TCContinuation<void> ()>> m_OnInitialFinishedSubscriptions;
		TCMap<CStr, TCActorFunctorWithID<TCContinuation<void> ()>> m_OnBackupStoppedSubscriptions;
		
		TCActorFunctor
			<
				TCContinuation<TCActorSubscriptionWithID<>>
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> &&_BackupInterface
					, CActorSubscription &&_ManifestFinished
					, CStr const &_BackupRoot
				)
			>
			m_fOnNewBackup
		;
		TCDelegatedActorInterface<CDistributedAppInterfaceBackupImplementation> m_BackupInterface;
		CActorSubscription m_BackupInterfaceSubscription;
		
		TCVector<TCContinuation<void>> m_SubscribeChangesContinuations;
		
		bool m_bRunningRetrySubscribe = false;
		bool m_bRerunRetrySubscribe = false;
		bool m_bBackupFinishedStarting = false;
		bool m_bInitialFinished = false;
		bool m_bStopped = false;
	};
}
