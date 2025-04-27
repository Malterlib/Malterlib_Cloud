
#pragma once

#include <Mib/Core/Core>
#include <Mib/Cloud/BackupManager>

namespace NMib::NCloud::NBackupManager
{
	struct CBackupSource : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;

		struct CCheckedOutDirectory
		{
			CStr m_Directory;
			CActorSubscription m_Subscription;
		};

		struct CInitialCommitResult
		{
			CBackupManagerBackup::CInitialBackupFinishedResult m_Result;
			CActorSubscription m_Subscription;
		};

		CBackupSource(CStr const &_Directory);
		~CBackupSource();

		TCFuture<CCheckedOutDirectory> f_CheckOutDirectory(CTime _Time);
		TCFuture<CBackupManager::CBackupInfo> f_GetInfo();

		TCFuture<CInitialCommitResult> f_InitialCommit
			(
				CStr _BackupID
				, CStr _Directory
				, CDirectoryManifest _Manifest
				, CBackupManagerBackup::EInitialBackupFinishedFlag _FinishedFlags
			)
		;
		TCFuture<void> f_Commit(CStr _BackupID, CStr _File, CBackupManagerBackup::CManifestChange _ManifestChange);
		TCFuture<void> f_CommitAppend
			(
				CStr _BackupID
				, CStr _File
				, uint64 _Position
				, CIOByteVector _Data
				, CBackupManagerBackup::CManifestChange _ManifestChange
			)
		;

	private:
		struct CCheckedOutState
		{
			void f_Cleanup();

			CStr m_Directory;
		};

		struct CBackup
		{
			CStr m_SourceDirectory;
			TCMap<CStr, CFile> m_AppendFiles;
		};

		TCFuture<void> fp_Destroy() override;
		void fp_Init();
		void fp_CloseFiles(CStr const &_FileName);
		void fp_SaveManifest();
		void fp_CleanupOldBackups();

		CDirectoryManifest mp_CurrentManifest;
		CStr mp_CurrentBackupID;
		CStr mp_LatestBackupID;

		CStr mp_Directory;
		CStr mp_LatestDirectory;

		TCMap<CStr, CCheckedOutState> mp_CheckedOutStates;
		CBackupManager::CBackupInfo mp_BackupInfo;

		TCMap<CStr, CBackup> mp_Backups;
	};
}
