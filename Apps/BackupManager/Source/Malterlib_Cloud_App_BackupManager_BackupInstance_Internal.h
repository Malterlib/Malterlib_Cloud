
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"

#include <Mib/File/RSync>
#include <Mib/Concurrency/ActorSubscription>

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
			
			NCryptography::CHashDigest_SHA256 m_ExpectedDigest;

			COnScopeExitShared m_SequenceSyncsCleanup;
			
			uint64 m_BytesTransferredIn = 0;
			uint64 m_BytesTransferredOut = 0;

			bool m_bFailedHash = false;
		};

		struct CSequencedSync
		{
			TCLinkedList<TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)>> m_Waiting;
		};

		struct CAppendFileState
		{
			CFile m_File;
			CHash_SHA256 m_Hash;
		};

		CInternal(CStr const &_Name, CTime const &_StartTime, CStr const &_ID, CStr const &_RootDirectory, bool _bForceNew, TCActor<CBackupSource> const &_BackupSource)
			: m_Name(_Name)
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

		void f_RunRSyncProtocol(CRSyncContext &_Context, CSecureByteVector &&_ServerPacket);
		CExceptionPointer f_CheckFileName(CStr const &_FileName, CDirectoryManifestFile **o_pManifestFile);
		CStr f_GetTempPath(CStr const &_Path);
		CStr f_GetCurrentPath(CStr const &_Path);
		CStr f_GetLatestPath(CStr const &_Path);
		TCFuture<TCActorSubscriptionWithID<>> f_StartRSyncShared
			(
				FRunRSyncProtocol &&_fRunProtocol
				, CStr const &_FileName
				, CStr const &_OldFileName
				, CStr const &_TempFileName
				, CStr const &_RelativeFileName
				, uint64 _FileLength
				, EDirectoryManifestSyncFlag _SyncFlags
				, CStr *o_pRSyncID
				, TCFunctionMovable<TCFuture<void> (TCAsyncResult<void> const &_Result)> &&_fOnDone
			 	, NCryptography::CHashDigest_SHA256 const &_ExpectedDigest
				, uint32 _ProtocolVersion
			)
		;
		TCFuture<void> f_CommitFile(CStr const &_File, CBackupManagerBackup::CManifestFile const &_ManifestFile);
		TCFuture<void> f_CommitManifestChange(CStr const &_FileName, CManifestChange const &_Change, CStr const &_Description);

		void f_InitBackupDirectory();

		void f_SequenceSyncs(CStr const &_FileName, TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun);
		void f_SequenceMultipleSyncs(TCFunctionMovable<void (COnScopeExitShared &&_pCleanup)> &&_fToRun, TCVector<CStr> const &_Files);
		void fp_RunSequencedSyncs(CStr const &_FileName);

		COnScopeExitShared f_FilePending(CStr const &_FileName);
		void f_OnPendingQuiescence(TCFunctionMutable<void ()> &&_fOnQuiescence);
		void fp_UpdatePendingQuiescence();

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
