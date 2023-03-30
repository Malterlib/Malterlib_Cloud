
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	CBackupInstance::CBackupInstance(CStr const &_Name, CTime const &_StartTime, CStr const &_ID, CStr const &_RootDirectory, bool _bForceNew, TCActor<CBackupSource> const &_BackupSource)
		: mp_pInternal(fg_Construct(_Name, _StartTime, _ID, _RootDirectory, _bForceNew, _BackupSource))
	{
	}

	CBackupInstance::~CBackupInstance()
	{
	}

	void CBackupInstance::CInternal::f_InitBackupDirectory()
	{
		m_RootBackupDirectory = fg_Format("{}/Backups/{}", m_RootDirectory, m_Name);
		m_BackupDirectory = fg_Format("{}/Sync_{}", m_RootBackupDirectory, m_ID);
		m_TempDirectory = fg_Format("{}/Temp/{}", m_RootBackupDirectory, m_ID);
	}

	CStr CBackupInstance::CInternal::f_GetCurrentPath(CStr const &_Path)
	{
		return CFile::fs_AppendPath(m_BackupDirectory, CFile::fs_AppendPath(CStr{"Files"}, _Path));
	}
	
	CStr CBackupInstance::CInternal::f_GetTempPath(CStr const &_Path)
	{
		return CFile::fs_AppendPath(m_TempDirectory, _Path);
	}
	
	CStr CBackupInstance::CInternal::f_GetLatestPath(CStr const &_Path)
	{
		return CFile::fs_AppendPath(m_RootBackupDirectory, CFile::fs_AppendPath(CStr{"Latest"}, _Path));
	}
	
	CExceptionPointer CBackupInstance::CInternal::f_CheckFileName(CStr const &_FileName, CDirectoryManifestFile **o_pManifestFile)
	{
		CStr Error;
		if (!CFile::fs_IsSafeRelativePath(_FileName, Error))
			return NException::fg_ExceptionPointer(DMibErrorInstance("The path cannot {}"_f << Error));

		if (!o_pManifestFile)
			return nullptr;
		
		auto *pManifestFile = m_Manifest.m_Files.f_FindEqual(_FileName);
		
		if (!pManifestFile)
		{
			DMibCloudBackupManagerDebugOut("NOT EXISTS: {}\n", _FileName);
			return fg_ExceptionPointer(DMibErrorInstance("File does not exists in manifest"));
		}
		
		*o_pManifestFile = pManifestFile; 
	
		return nullptr;
	}

	TCFuture<void> CBackupInstance::fp_Destroy()
	{
		auto &Internal = *mp_pInternal;

		if (Internal.m_BackupSourceSubscription)
			co_await Internal.m_BackupSourceSubscription->f_Destroy();

		co_return {};
	}
	
	auto CBackupInstance::f_StartBackup() -> TCFuture<CStartBackupResult>
	{
		auto &Internal = *mp_pInternal;

		if (!Internal.m_bManifestSyncDone)
			co_return DMibErrorInstance("You need to rsync manifest with f_StartManifestRSync before starting backup");

		if (Internal.m_bBackupStarted)
			co_return DMibErrorInstance("Backup already started");

		auto CaptureScope = co_await g_CaptureExceptions.f_Specific<CExceptionFile>();

		TCBinaryStreamFile<> Stream;
		Stream.f_Open(CFile::fs_AppendPath(Internal.m_BackupDirectory, "Manifest.bin"), EFileOpen_Read | EFileOpen_ShareAll);
		Stream >> Internal.m_Manifest;

		TCSet<CStr> ManifestFiles;
		for (auto &File : Internal.m_Manifest.m_Files)
		{
			auto &FileName = Internal.m_Manifest.m_Files.fs_GetKey(File);
			CStr ManifestError;
			if (!CBackupManagerBackup::fs_ManifestFileValid(FileName, File, ManifestError))
				DMibErrorFile("Manifest file '{}' invalid: {}"_f << FileName << ManifestError);

			ManifestFiles[FileName];
		}

		CStartBackupResult BackupResult;

		for (auto &File : Internal.m_Manifest.m_Files)
		{
			if (File.m_Attributes & (EFileAttrib_Directory | EFileAttrib_Link))
				continue;

			CStr FileName = Internal.f_GetCurrentPath(File.f_GetFileName());
			CStr OldFileName = Internal.f_GetLatestPath(File.f_GetFileName());

			if (!CFile::fs_FileExists(FileName))
			{
				CFile::CFileChecksumState_SHA256 ChecksumState;

				try
				{
					if (CFile::fs_FileExists(OldFileName))
					{
						CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));

						bool bDuplicatedFile = false;

						if (File.m_Flags & EDirectoryManifestSyncFlag_Append)
						{
							CStr SourceFileName = OldFileName;

							if (CFile::fs_TryDuplicateFile(OldFileName, FileName))
								bDuplicatedFile = true;
							else
							{
								if (CFile::fs_GetFileChecksum_SHA256(SourceFileName, &ChecksumState) == File.m_Digest)
								{
									CFile OutFile;
									OutFile.f_Open(FileName, EFileOpen_Write | EFileOpen_ShareAll, EFileAttrib_UserRead | EFileAttrib_UserWrite);
									ChecksumState.m_pFile->f_SetPosition(0);
									CFile::fs_CopyFileRaw(*ChecksumState.m_pFile, OutFile);
									continue;
								}
							}
						}
						else
						{
							CFile::fs_CreateHardLink(OldFileName, FileName);
							bDuplicatedFile = true;
						}

						if (bDuplicatedFile)
						{
							auto Cleanup = g_OnScopeExit / [&]
								{
									if (CFile::fs_FileExists(FileName))
										CFile::fs_DeleteFile(FileName);
								}
							;
							if (File.m_Flags & EDirectoryManifestSyncFlag_Append)
								Cleanup.f_Clear();

							if (CFile::fs_GetFileChecksum_SHA256(FileName, &ChecksumState) == File.m_Digest)
							{
								Cleanup.f_Clear();
								continue;
							}
						}
					}
				}
				catch (CExceptionFile const &_Exception)
				{
					(void)_Exception;
					DMibLogWithCategory(Mib/Cloud/BackupManager, Info, "Hardlink file optimization failed: {}", _Exception);
				}

				BackupResult.m_FilesNotUpToDate[File.f_GetFileName()] = ChecksumState.m_Hash.f_GetDigest();
				continue;
			}

			auto Hash = CFile::fs_GetFileChecksum_SHA256(FileName);
			if (Hash == File.m_Digest)
				continue;

			BackupResult.m_FilesNotUpToDate[File.f_GetFileName()] = Hash;
		}

		CFile::fs_CreateDirectory(Internal.m_BackupDirectory);
		CFile::fs_Touch(fg_Format("{}/{tst.,tsb_}.timestamp", Internal.m_BackupDirectory, Internal.m_StartTime));

#ifdef DMibDebug
		CFile::fs_WriteStringToFile(CFile::fs_AppendPath(Internal.m_BackupDirectory, "Manifest.json"), Internal.m_Manifest.f_ToJSON().f_ToString());
#endif

		Internal.m_bBackupStarted = true;

		co_return fg_Move(BackupResult);
	}

	TCFuture<void> CBackupInstance::CInternal::f_CommitManifestChange(CStr const &_FileName, CManifestChange const &_Change, CStr const &_Description)
	{
#if defined DMibContractConfigure_CheckEnabled
		CStr ManifestError;
		DMibCheck(CBackupManagerBackup::fs_ManifestChangeValid(_FileName, _Change, ManifestError));
#endif
		CBackupManagerBackup::fs_ApplyManifestChange(m_Manifest, _FileName, _Change);

		if (!m_bInitialBackupFinished)
			co_return {};

		auto Result = co_await m_BackupSource(&CBackupSource::f_Commit, m_ID, _FileName, _Change).f_Wrap();
		if (!Result)
		{
			DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to commit file {}: {}", _Description, Result.f_GetExceptionStr());
			co_return DMibErrorInstance("Failed to commit {} file in backup (check backup manager log)"_f << _Description);
		}

		co_return {};
	}

	TCFuture<void> CBackupInstance::CInternal::f_CommitFile(CStr const &_File, CBackupManagerBackup::CManifestFile const &_ManifestFile)
	{
#if defined DMibContractConfigure_CheckEnabled
		CStr ManifestError;
		DMibCheck(CBackupManagerBackup::fs_ManifestFileValid(_File, _ManifestFile, ManifestError));
#endif
		CBackupManagerBackup::CManifestChange ManifestChange;

		if (auto pFile = m_Manifest.m_Files.f_FindEqual(_File))
		{
			*pFile = _ManifestFile;
			ManifestChange = CBackupManagerBackup::CManifestChange_Change{_ManifestFile};
		}
		else
		{
			m_Manifest.m_Files[_File] = _ManifestFile;
			CBackupManagerBackup::CManifestChange_Add Add;
			Add.m_ManifestFile = _ManifestFile;
			ManifestChange = fg_Move(Add);
		}

		if (!m_bInitialBackupFinished)
			co_return {};

		co_return co_await m_BackupSource(&CBackupSource::f_Commit, m_ID, _File, fg_Move(ManifestChange));
	}

	auto CBackupInstance::f_InitialBackupFinished(EInitialBackupFinishedFlag _FinishedFlags) -> TCFuture<CInitialBackupFinishedResult>
	{
		TCPromise<CInitialBackupFinishedResult> Promise;

		auto &Internal = *mp_pInternal;

		Internal.f_OnPendingQuiescence
			(
				[Promise, this, _FinishedFlags]
				{
					// When we get here no rsyncs should be running, only appends can be queued. Guard against this by sequencing against all files in manifest

					auto &Internal = *mp_pInternal;
					TCVector<CStr> FilesToSynchronize;
					for (auto &File : Internal.m_Manifest.m_Files)
						FilesToSynchronize.f_Insert(File.f_GetFileName());

					Internal.f_SequenceMultipleSyncs
						(
							[Promise, this, _FinishedFlags](COnScopeExitShared &&_pCleanup)
							{
								auto &Internal = *mp_pInternal;
								Internal.m_bInitialBackupFinished = true;
								DMibLogWithCategory(Mib/Cloud/BackupManager, Info, "({} - {tc5}) Initial backup finished, committing to latest", Internal.m_Name, Internal.m_StartTime);
								Internal.m_BackupSource(&CBackupSource::f_InitialCommit, Internal.m_ID, Internal.f_GetCurrentPath(""), fg_TempCopy(Internal.m_Manifest), _FinishedFlags)
									> Promise / [=](CBackupSource::CInitialCommitResult &&_Result)
									{
										(void)_pCleanup;
										auto &Internal = *mp_pInternal;
										Internal.m_BackupSourceSubscription = fg_Move(_Result.m_Subscription);

										DMibLogWithCategory(Mib/Cloud/BackupManager, Debug, "({} - {tc5}) Committed to latest, finishing", Internal.m_Name, Internal.m_StartTime);

										Promise.f_SetResult(_Result.m_Result);
									}
								;
							}
							, FilesToSynchronize
						)
					;

				}
			)
		;

		return Promise.f_MoveFuture();
	}
}
