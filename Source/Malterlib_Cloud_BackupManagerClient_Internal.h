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
using namespace NMib::NStorage;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NCryptography;

namespace NMib::NCloud
{
	static constexpr NFile::EFileOpen gc_ChecksumFileFlags = NFile::EFileOpen_Read | NFile::EFileOpen_ShareAll | NFile::EFileOpen_NoLocalCache | NFile::EFileOpen_ShareBypass;

	struct CBackupManagerClient_ChecksumState
	{
		uint64 m_Position = 0;
		CHash_SHA256 m_DigestState;
	};

	struct CBackupManagerClient::CInternal : public CActorInternal
	{
		CInternal
			(
				CBackupManagerClient *_pThis
				, CConfig const &_Config
				, TCActor<CDistributedActorTrustManager> const &_TrustManager
				, TCActorFunctor
				<
					TCFuture<TCActorSubscriptionWithID<>>
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
			bool m_bToBeRemoved = false;
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
			bool m_bChecksumValid = false;
			CBackupManagerClient_ChecksumState m_ChecksumState;
		};

		struct CAppendFileState
		{
			CFile m_File;
			CBackupManagerClient_ChecksumState m_ChecksumState;
			CUniqueFileIdentifier m_FileID;
			CDirectoryManifestFile m_ManifestFile;
			bool m_bIsLink = false;
			bool m_bIsValid = false;
		};

		struct CNotifacitonSubscription
		{
			CBackupManagerClient::ENotification m_Notifications;
			TCActorFunctor<TCFuture<void> (CHostInfo const &_RemoteHost, CBackupManagerClient::CNotification &&_Notification)> m_fOnNotification;
		};

		struct CDistributedAppInterfaceBackupImplementation : public CDistributedAppInterfaceBackup
		{
			TCFuture<void> f_AppendManifest(NFile::CDirectoryManifestConfig const &_Config) override;
			TCFuture<TCActorSubscriptionWithID<>> f_SubscribeInitialFinished(TCActorFunctorWithID<TCFuture<void> ()> &&_fOnInitialFinished) override;
			TCFuture<TCActorSubscriptionWithID<>> f_SubscribeBackupStopped(TCActorFunctorWithID<TCFuture<void> ()> &&_fOnStopped) override;

			CBackupManagerClient *m_pThis = nullptr;
		};

		struct CNotificationAndHost
		{
			CNotification m_Notification;
			CHostInfo m_RemoteHost;
		};

		struct CRunningInstance
		{
			TCActor<NPrivate::CBackupManagerClient_Instance> m_Instance;
			bool m_bSentActive = false;
		};

		void f_Construct(TCActor<CActorDistributionManager> const &_DistributionManager);
		void f_NewBackupKey();
		void f_RunBackup();
		void f_Subscribe();
		void f_BackupFinishedStarting();
		void f_BackupInstance_ReportFinishedStarting(TCActor<NPrivate::CBackupManagerClient_Instance> const &_BackupInstance);
		void f_ReportBackupError(CStr const &_Error, bool _bFatal);

		TCFuture<void> f_SubscribeChanges();
		TCFuture<void> f_RetrySubscribeChanges();
		void f_NewPathWatched(CStr const &_Path);
		void f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification, bool _bDirty);
		bool f_IsPathInManifest(CStr const &_Path, CStr &o_FileName);
		static void fs_CheckDestroy(TCSharedPointer<NAtomic::TCAtomic<bool>> const &_pDestroyed);

		CFileChangeNotificationActor::CCoalesceSettings f_CoalesceSettings();

		TCFuture<CUpdateManifestResult> f_UpdateManifest(CStr const &_FileName, CStr const &_OriginalFileName, bool _bDirtyHint);

		COnScopeExitShared f_MarkInstancesActive();

		CBackupManagerClient *m_pThis = nullptr;
		TCSharedPointer<NAtomic::TCAtomic<bool>> m_pDestroyed;
		CConfig m_Config;
		TCActor<CDistributedActorTrustManager> m_TrustManager;

		TCActor<CSeparateThreadActor> m_FileActor;
		CDirectoryManifest m_Manifest; // Kept up to date
		TCMap<CStr, CBackupManagerClient_ChecksumState> m_ChecksumState;
		TCMap<CStr, CUniqueFileIdentifier> m_ManifestFileIDs;
		TCMap<CStr, TCSharedPointer<CAppendFileState>> m_AppendStates;

		TCActor<CFileChangeNotificationActor> m_FileChangeNotificationsActor;
		TCMap<CStr, CWatchedPath> m_WatchedPaths;
		TCMap<CStr, CWatchedPath> m_WatchedPathsMissing;

		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotifications;
		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotificationsMissing;

		TCTrustedActorSubscription<CBackupManager> m_BackupManagers;

		TCMap<TCWeakActor<CBackupManager>, CRunningInstance> m_RunningBackupInstances;

		TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker = fg_Construct();

		CBackupManager::CBackupKey m_BackupKey;

		mint m_nActive = 0;

		mint m_FileNotificationSequence = 1;
		mint m_LastSeenNotificationSequence = 0;

		TCMap<CStr, CNotifacitonSubscription> m_NotificationSubscriptions;
		TCMap<CStr, TCActorFunctorWithID<TCFuture<void> ()>> m_OnInitialFinishedSubscriptions;
		TCMap<CStr, TCActorFunctorWithID<TCFuture<void> ()>> m_OnBackupStoppedSubscriptions;

		TCActorFunctor
			<
				TCFuture<TCActorSubscriptionWithID<>>
				(
					TCDistributedActorInterfaceWithID<CDistributedAppInterfaceBackup> &&_BackupInterface
					, CActorSubscription &&_ManifestFinished
					, CStr const &_BackupRoot
				)
			>
			m_fOnNewBackup
		;
		TCDistributedActorInstance<CDistributedAppInterfaceBackupImplementation> m_BackupInterface;
		CActorSubscription m_BackupInterfaceSubscription;

		TCVector<TCPromise<void>> m_SubscribeChangesPromises;

		TCMap<ENotification, CNotificationAndHost> m_LastNotification;

		bool m_bRunningRetrySubscribe = false;
		bool m_bRerunRetrySubscribe = false;
		bool m_bBackupFinishedStarting = false;
		bool m_bInitialFinished = false;
		bool m_bStopped = false;
		bool m_bStarted = false;
		bool m_bInitialSubscribeDone = false;
	};
}
