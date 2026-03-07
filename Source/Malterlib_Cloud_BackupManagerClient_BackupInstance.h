// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include <Mib/Cloud/BackupManager>
#include <Mib/File/RSync>

#include <Mib/Concurrency/ActorSequencerActor>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NContainer;
using namespace NMib::NStorage;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NCryptography;
using namespace NMib::NStream;
using namespace NMib::NFunction;

namespace NMib::NCloud::NPrivate
{
	struct CBackupManagerClient_Instance : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;

		CBackupManagerClient_Instance
			(
				TCDistributedActor<CBackupManager> const &_BackupManager
				, CDirectoryManifest const &_Manifest
				, TCMap<CStr, CBackupManagerClient_ChecksumState> const &_InitialChecksumState
				, CBackupManagerClient::CConfig const &_Config
				, CTrustedActorInfo const &_ActorInfo
				, TCWeakActor<CBackupManagerClient> const &_BackupManagerClient
				, CBackupManager::CBackupKey const &_BackupKey
				, bool _bFinishedStarting
			)
		;
		~CBackupManagerClient_Instance();

		void f_BackupFinishedStarting();
		void f_MarkActive(bool _bActive);
		TCFuture<void> f_ManifestChanged
			(
				CStr _FileName
				, CBackupManagerBackup::CManifestChange _ManifestChange
				, bool _bDirty
				, CBackupManagerClient_ChecksumState _ChecksumState
			)
		;

	private:
		struct CRunningSyncState
		{
			CRunningSyncState();
			~CRunningSyncState();

			CIntrusiveRefCountWithWeak m_RefCount;
			CStr m_FileName;
			TCBinaryStreamFile<> m_File;
			CBinaryStreamSubStream<> m_LimitedFile;
			TCUniquePointer<CRSyncServer> m_pRSyncServer;
			COnScopeExitShared m_pOnScopeExit;
			TCSharedPointer<CCanDestroyTracker> m_pCanDestroyTracker;
			CDirectoryManifestFile m_ManifestFile;
			uint32 m_PendingQueue = 0;
		};

		struct CManifestSyncState
		{
			CIntrusiveRefCountWithWeak m_RefCount;
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

			CActorSubscription m_RSyncSubscription;
			mint m_SyncSequence = 0;

			CBackupManagerClient::CFileTransferStats m_TransferStats;
			NTime::CClock m_Clock{true};

			CDirectoryManifestFile m_ManifestFile;

			COnScopeExitShared m_pSequenceSyncsScope;

			bool m_bFinished = false;
		};

		struct CManifestChangeInfo
		{
			CBackupManagerBackup::CManifestChange m_ManifestChange;
			TCPromise<void> m_Promise;
			bool m_bDirty = false;
		};

		struct CAppendFileCache
		{
			CFile m_File;
			CUniqueFileIdentifier m_FileID;
		};

		struct CAppendFileState : public CBackupManagerClient_ChecksumState
		{
			CBackupManagerClient_ChecksumState &f_ChecksumState()
			{
				return *this;
			}
			bool m_bDirty = true;
		};

		struct CSequencedSync
		{
			mint m_nReading = 0;
			mint m_nWriting = 0;

			TCLinkedList<TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)>> m_ReadWaiting;
			TCLinkedList<TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)>> m_WriteWaiting;
		};

		struct CPendingManifestChange
		{
			CStr m_FileName;
			CManifestChangeInfo m_Info;
		};

		TCFuture<void> fp_Destroy() override;
		void fp_StartBackup();
		void fp_BackupNotification(CBackupManagerClient::CNotification &&_Notification);
		void fp_ReportBackupError(CStr const &_Error, bool _bFatal);

		TCFuture<void> fp_SyncManifest();

		void fp_ProcessBackupQueue();
		void fp_RSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, CPendingBackupFile &_PendingFile, mint _SyncSequence);
		void fp_SendAppendSyncFile
			(
				TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState
				, TCPromise<bool> const &_Promise
				, uint64 _Length
				, TCSharedPointer<CAppendFileCache> const &_pFile
				, bool _bForceSync
			)
		;
		void fp_AppendSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, mint _SyncSequence);

		void fp_NewPendingFile(CStr const &_FileName);
		void fp_FileFinished(CStr const &_FileName);
		void fp_ProcessManifestChange(CStr const &_FileName, CManifestChangeInfo const &_ManifestChange);
		TCSharedPointer<CAppendFileCache> fp_GetAppendFileCache(CStr const &_FileName);
		void fp_CheckInitialBackupFinished();
		void fp_CheckQuiescent();
		void fp_NotQuiescent();
		void fp_ReportHashMismatch(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, CPendingBackupFile &_PendingFile);
		void fp_ReportRetry(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState);

		void fp_SequenceWriteSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun);
		void fp_SequenceReadSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun);
		void fp_SequenceMultipleSyncs
			(
				TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun
				, TCVector<CStr> const &_WriteFiles
				, TCVector<CStr> const &_ReadFiles
			)
		;
		void fp_RunSequencedSyncs(CStr const &_FileName);

		TCSharedPointer<CCanDestroyTracker> mp_pCanDestroyTracker = fg_Construct();

		TCDistributedActor<CBackupManager> mp_BackupManager;
		TCWeakActor<CBackupManagerClient> mp_BackupManagerClient;
		CBackupManagerClient::CConfig mp_Config;
		CTrustedActorInfo mp_ActorInfo;

		CDirectoryManifest mp_Manifest;

		TCSharedPointer<CManifestSyncState> mp_pManifestSyncState;

		TCMap<CStr, CSequencedSync> mp_SequencedSyncs;

		CBackupManager::CBackupKey mp_BackupKey;
		TCDistributedActorInterface<CBackupManagerBackup> mp_Backup;
		TCSet<CStr> mp_InitialBackupPendingAdded;
		TCSet<CStr> mp_InitialBackupPending;

		TCMap<CStr, CPendingBackupFile> mp_PendingFiles;
		DMibListLinkDS_List(CPendingBackupFile, m_Link) mp_PendingFilesQueue;

		TCVector<CPendingManifestChange> mp_PendingManifestChanges;

		TCMap<CStr, TCSharedPointer<CAppendFileCache>> mp_AppendFileCache;
		TCMap<CStr, CAppendFileState> mp_AppendFileState;

		mint mp_nRunningSyncs = 0;
		mint mp_nMaxRunningSyncs = 8;
		mint mp_SyncSequence = 0;
		mint mp_nPendingManifestChanges = 0;

		bool mp_bBackupStarted = false;
		bool mp_bBackupStartFailed = false;
		bool mp_bInitialBackupFinished = false;
		bool mp_bInitialBackupCommitted = false;
		bool mp_bFinishedStarting = false;
		bool mp_bQuiescent = false;
		bool mp_bClientActive = false;
	};
}
