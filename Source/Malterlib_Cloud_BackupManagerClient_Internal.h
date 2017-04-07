// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>

#include "Malterlib_Cloud_BackupManagerClient.h"

#include <Mib/Cloud/BackupManager>
#include <Mib/File/ChangeNotificationActor>
#include <Mib/Concurrency/DistributedActorTrustManager>

using namespace NMib; 
using namespace NMib::NConcurrency; 
using namespace NMib::NContainer; 
using namespace NMib::NPtr; 
using namespace NMib::NFile; 
using namespace NMib::NStr; 

namespace NMib::NCloud
{
	struct CBackupManagerClient::CInternal
	{
		CInternal(CBackupManagerClient *_pThis, CConfig const &_Config, TCActor<CDistributedActorTrustManager> const &_TrustManager);
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
			CBackupManagerBackup::CManifestFile m_ManifestFile;
			bool m_bAdded = false;
		};
		
		struct CUpdateManifestResult
		{
			CBackupManagerBackup::CManifestFile m_ManifestFile;
			TCMap<CStr, CUpdatedDirectory> m_UpdatedDirectories;
			bool m_bExists = false;
			bool m_bRemoved = false;
			bool m_bAdded = false;
		};
		
		void f_Construct();
		void f_NewBackupKey();
		void f_RunBackup();
		void f_Subscribe();
		TCContinuation<void> f_SubscribeChanges();
		TCContinuation<void> f_RetrySubscribeChanges();
		void f_OnFileChanged(CFileChangeNotification::CNotification const &_Notification);
		bool f_IsPathInManifest(CStr const &_Path);
		
		TCContinuation<CUpdateManifestResult> f_UpdateManifest(CStr const &_FileName);
		
		static void fs_GetManifestFileProperties(CConfig const &_Config, CStr const &_FileName, CBackupManagerBackup::CManifestFile &o_ManifestFile);
		static CBackupManagerBackup::CManifest fs_GetManifest(CConfig const &_Config);
		static void fs_ValidateWildcard(NStr::CStr const &_Wildcard, bool _bIsFileSearch);
		template <typename tf_CContainer>
		static void fs_ValidateWildcards(tf_CContainer const &_Container, bool _bIsFileSearch);
		template <typename tf_CContainer>
		static bool fs_MatchesAnyWildcard(CStr const &_String, tf_CContainer const &_Container);
		static CStr fs_ParseWildcard(CStr const &_Wildcard, bool &o_bRecursive);
		
		CBackupManagerClient *m_pThis = nullptr;
		CConfig m_Config;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		
		TCActor<CSeparateThreadActor> m_FileActor;
		CBackupManagerBackup::CManifest m_Manifest; // Kept up to date

		TCActor<CFileChangeNotificationActor> m_FileChangeNotificationsActor;
		TCMap<CStr, CWatchedPath> m_WatchedPaths;
		TCMap<CStr, CWatchedPath> m_WatchedPathsMissing;
		
		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotifications;
		TCVector<CFileChangeNotification::CNotification> m_PendingFileChangeNotificationsMissing;
		
		TCTrustedActorSubscription<CBackupManager> m_BackupManagers;
		
		TCMap<TCWeakActor<CBackupManager>, TCActor<NPrivate::CBackupManagerClient_Instance>> m_RunningBackupInstances;
		
		TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker = fg_Construct();
		
		CBackupManager::CBackupKey m_BackupKey;
		
		bool m_bRunningRetrySubscribe = false;
		bool m_bRerunRetrySubscribe = false;
	};
}

#include "Malterlib_Cloud_BackupManagerClient_Internal.hpp"
