// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Cloud/BackupManager>
#include <Mib/File/RSync>

#include <Mib/Concurrency/ActorSequencer>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

using namespace NMib; 
using namespace NMib::NConcurrency; 
using namespace NMib::NContainer; 
using namespace NMib::NPtr; 
using namespace NMib::NFile; 
using namespace NMib::NStr; 
using namespace NMib::NDataProcessing; 
using namespace NMib::NStream;

namespace NMib::NCloud::NPrivate
{
	struct CBackupManagerClient_Instance : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;
		
		CBackupManagerClient_Instance
			(
				TCDistributedActor<CBackupManager> const &_BackupManager
				, CDirectoryManifest const &_Manifest
				, CBackupManagerClient::CConfig const &_Config
				, CTrustedActorInfo const &_ActorInfo
				, TCWeakActor<CBackupManagerClient> const &_BackupManagerClient
				, CBackupManager::CBackupKey const &_BackupKey
				, bool _bFinishedStarting
			)
		;
		~CBackupManagerClient_Instance();
		
		void f_BackupFinishedStarting();
		TCContinuation<void> f_ManifestChanged(CStr const &_FileName, CBackupManagerBackup::CManifestChange const &_ManifestChange, bool _bDirty);
		
	private:
		
		struct CRunningSyncState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			CRunningSyncState();
			~CRunningSyncState();

			CStr m_FileName;
			CStr m_OriginalFileName;
			TCBinaryStreamFile<> m_File;
			TCUniquePointer<CRSyncServer> m_pRSyncServer;
			COnScopeExitShared m_pOnScopeExit;
			TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker;
			uint32 m_PendingQueue = 0;
			bool m_bAborted = false;
		};
		
		struct CManifestSyncState : public TCSharedPointerIntrusiveBase<ESharedPointerOption_SupportWeakPointer>
		{
			CBinaryStreamMemory<> m_ManifestStream;
			TCUniquePointer<CRSyncServer> m_pRSyncServer;
			CActorSubscription m_RSyncSubscription;
			bool m_bDone = false;
		};
		
		struct CPendingBackupFile
		{
			CStr const &f_GetFileName() const
			{
				return TCMap<CStr, CPendingBackupFile>::fs_GetKey(*this);
			}
			
			~CPendingBackupFile();
			
			DMibListLinkDS_Link(CPendingBackupFile, m_Link);
			
			TCWeakPointer<CRunningSyncState> m_pRunningState;
			TCVector<TCContinuation<void>> m_StartedContinuations;
			
			CActorSubscription m_RSyncSubscription;
			mint m_SyncSequence = 0;
			
			EDirectoryManifestSyncFlag m_SyncFlags = EDirectoryManifestSyncFlag_None;
			CStr m_OriginalPath;
			
			bool m_bReschedule = false;
			bool m_bFinished = false;
		};
		
		struct CManifestChangeInfo
		{
			CBackupManagerBackup::CManifestChange m_ManifestChange;
			TCContinuation<void> m_Continuation;
			bool m_bDirty = false;
		};
		
		struct CAppendFileCache
		{
			CFile m_File;
			CUniqueFileIdentifier m_FileID;
		};

		struct CAppendFileState
		{
			uint64 m_Position = 0;
			CHash_SHA256 m_DigestState;
			bool m_bDirty = true;
		};
		
		TCContinuation<void> fp_Destroy() override;
		void fp_StartBackup();
		void fp_BackupNotification(CBackupManagerClient::CNotification &&_Notification);
		void fp_ReportBackupFailed(CStr const &_Error);

		TCContinuation<void> fp_SyncManifest();
		
		void fp_ProcessBackupQueue();
		void fp_RSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, CPendingBackupFile &_PendingFile, mint _SyncSequence);
		void fp_SendAppendSyncFile
			(
				TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState
				, TCContinuation<bool> const &_Continuation
				, uint64 _Length
				, TCSharedPointer<CAppendFileCache> const &_pFile
			)
		;
		void fp_AppendSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, mint _SyncSequence);

		void fp_NewPendingFile(CStr const &_FileName);
		void fp_FileFinished(CStr const &_FileName);
		void fp_ProcessManifestChange(CStr const &_FileName, CManifestChangeInfo const &_ManifestChange);
		TCContinuation<void> fp_AbortPendingFile(CStr const &_FileName);
		TCSharedPointer<CAppendFileCache> fp_GetAppendFileCache(CStr const &_FileName);
		void fp_CheckInitialBackupFinished();
		
		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker = fg_Construct();
		
		TCDistributedActor<CBackupManager> mp_BackupManager;
		TCWeakActor<CBackupManagerClient> mp_BackupManagerClient;
		CBackupManagerClient::CConfig mp_Config;
		CTrustedActorInfo mp_ActorInfo;
		
		CDirectoryManifest mp_Manifest;
		
		TCSharedPointer<CManifestSyncState> mp_pManifestSyncState;
		
		CBackupManager::CBackupKey mp_BackupKey;
		TCDistributedActorInterface<CBackupManagerBackup> mp_Backup;
		TCSet<CStr> mp_InitialBackupPendingAdded;
		TCSet<CStr> mp_InitialBackupPending;
		
		TCMap<CStr, CPendingBackupFile> mp_PendingFiles;
		DMibListLinkDS_List(CPendingBackupFile, m_Link) mp_PendingFilesQueue;

		TCMap<CStr, TCVector<CManifestChangeInfo>> mp_PendingManifestChanges;
		
		TCMap<CStr, TCSharedPointer<CAppendFileCache>> mp_AppendFileCache;
		TCMap<CStr, CAppendFileState> mp_AppendFileState;
		
		TCActorSequencer<void> mp_FileManifestSequencer;

		mint mp_nRunningSyncs = 0;
		mint mp_nMaxRunningSyncs = 8;
		mint mp_SyncSequence = 0;
		
		bool mp_bBackupStarted = false;
		bool mp_bBackupStartFailed = false;
		bool mp_bInitialBackupFinished = false;
		bool mp_bFinishedStarting = false;
	};
}
