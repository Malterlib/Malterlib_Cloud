
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"

#include <Mib/File/RSync>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ActorSequencerActor>

namespace NMib::NCloud::NBackupManager
{
	struct CBackupInstance::CInternal : public NConcurrency::CActorInternal
	{
		struct CRSyncContext
		{
			CStr const &f_GetSyncID() const
			{
				return TCMap<CStr, CRSyncContext>::fs_GetKey(*this);
			}

			CStr m_RelativeFileName;
			CStr m_AbsoluteFileName;
			uint64 m_FileLength = 0;

			TCBinaryStreamFile<> m_File;
			TCBinaryStreamFile<> m_SourceFile;
			TCBinaryStreamFile<> m_TempFile;

			TCUniquePointer<CRSyncClient> m_pClient;

			FRunRSyncProtocol m_fRunProtocol;

			TCVector<CStr> m_TempFileNames;

			TCOptional<NCryptography::CHashDigest_SHA256> m_ExpectedDigest;

			CActorSubscription m_SequenceSyncsCleanup;

			uint64 m_BytesTransferredIn = 0;
			uint64 m_BytesTransferredOut = 0;

			bool m_bFailedHash = false;
		};

		struct CSequencedSync
		{
			CSequencer m_Sequencer{"Backup Instance Sync"};
		};

		struct CAppendFileState
		{
			CFile m_File;
			CHash_SHA256 m_Hash;
		};

		CInternal
			(
				CBackupInstance *_pThis
				, CStr const &_Name
				, CTime const &_StartTime
				, CStr const &_ID
				, CStr const &_RootDirectory
				, bool _bForceNew
				, TCActor<CBackupSource> const &_BackupSource
			)
			: m_pThis(_pThis)
			, m_Name(_Name)
			, m_StartTime(_StartTime)
			, m_ID(_ID)
			, m_RootDirectory(_RootDirectory)
			, m_bForceNew(_bForceNew)
			, m_BackupSource(_BackupSource)
		{
			g_Dispatch / [this]
				{
					f_InitBackupDirectory();
				}
				> [](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						DMibLogWithCategory(Mib/Cloud/BackupManager, Debug, "Failed to init backup directory: {}", _Result.f_GetExceptionStr());
				}
			;
		}

		void f_RunRSyncProtocol(CRSyncContext &_Context, CIOByteVector &&_ServerPacket);
		CExceptionPointer f_CheckFileName(CStr const &_FileName, CDirectoryManifestFile **o_pManifestFile);
		CStr f_GetTempPath(CStr const &_Path);
		CStr f_GetCurrentPath(CStr const &_Path);
		CStr f_GetLatestPath(CStr const &_Path);
		TCFuture<TCActorSubscriptionWithID<>> f_StartRSyncShared
			(
				FRunRSyncProtocol _fRunProtocol
				, CStr _FileName
				, CStr _OldFileName
				, CStr _TempFileName
				, CStr _RelativeFileName
				, uint64 _FileLength
				, EDirectoryManifestSyncFlag _SyncFlags
				, CStr *o_pRSyncID
				, TCFunctionMovable<TCFuture<void> (TCAsyncResult<void> const &_Result)> _fOnDone
				, TCOptional<NCryptography::CHashDigest_SHA256> _ExpectedDigest
				, uint32 _ProtocolVersion
			)
		;
		TCFuture<void> f_CommitFile(CStr _File, CBackupManagerBackup::CManifestFile _ManifestFile);
		TCFuture<void> f_CommitManifestChange(CStr _FileName, CManifestChange _Change, CStr _Description);

		void f_InitBackupDirectory();

		TCFuture<CActorSubscription> f_SequenceSyncs(CStr _FileName);
		TCFuture<CActorSubscription> f_SequenceMultipleSyncs(TCVector<CStr> _Files);

		COnScopeExitShared f_FilePending(CStr const &_FileName);
		void f_OnPendingQuiescence(TCFunctionMutable<void ()> &&_fOnQuiescence);
		void fp_UpdatePendingQuiescence();

		CBackupInstance *m_pThis = nullptr;

		CStr m_Name;
		CTime m_StartTime;
		CStr m_ID;

		TCActor<CBackupSource> m_BackupSource;

		CStr m_RootDirectory;
		CStr m_BackupDirectory;
		CStr m_RootBackupDirectory;
		CStr m_TempDirectory;

		CDirectoryManifest m_Manifest;

		TCMap<CStr, CAppendFileState> m_AppendStates;
		TCMap<CStr, CRSyncContext> m_RSyncContexts;

		TCMap<CStr, CSequencedSync> m_SequencedSyncs;
		TCMap<CStr, zmint> m_PendingSyncs;
		TCLinkedList<TCFunctionMutable<void ()>> m_PendingSyncsQuiescence;

		CStr m_ManifestRSyncID;

		CActorSubscription m_BackupSourceSubscription;

		bool m_bManifestSyncStarted = false;
		bool m_bManifestSyncDone = false;
		bool m_bBackupStarted = false;
		bool m_bManifestRSyncDone = false;
		bool m_bForceNew = false;
		bool m_bInitialBackupFinished = false;
	};
}
