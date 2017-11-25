
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

		TCContinuation<CCheckedOutDirectory> f_CheckOutDirectory(CTime const &_Time);
		TCContinuation<CBackupManager::CBackupInfo> f_GetInfo();

		TCContinuation<CInitialCommitResult> f_InitialCommit
			(
			 	CStr const &_BackupID
			 	, CStr const &_Directory
			 	, CDirectoryManifest &&_Manifest
			 	, CBackupManagerBackup::EInitialBackupFinishedFlag _FinishedFlags
			)
		;
		TCContinuation<void> f_Commit(CStr const &_BackupID, CStr const &_File, CBackupManagerBackup::CManifestChange const &_ManifestChange);
		TCContinuation<void> f_CommitAppend
			(
			 	CStr const &_BackupID
			 	, CStr const &_File
			 	, uint64 _Position
			 	, CSecureByteVector &&_Data
			 	, CBackupManagerBackup::CManifestChange &&_ManifestChange
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

		TCContinuation<void> fp_Destroy() override;
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
