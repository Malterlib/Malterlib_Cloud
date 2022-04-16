
#include "Malterlib_Cloud_App_BackupManager_BackupSource.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"

#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NBackupManager
{
	TCFuture<TCVector<CStr>> CBackupManagerServer::fp_EnumBackupSourcesFromDisk()
	{
		co_return co_await
			(
			 	g_Dispatch(fp_GetQueryFileActor()) / [RootDirectory = mp_AppState.m_RootDirectory]
			 	{
					CStr FindPath = RootDirectory + "/Backups";
					CFile::CFindFilesOptions FindOptions(FindPath + "/*_*", false);
					FindOptions.m_AttribMask = EFileAttrib_Directory;
					auto FoundFiles = CFile::fs_FindFiles(FindOptions);
					TCVector<CStr> BackupSources;
					for (auto &File : FoundFiles)
					{
						CStr BackupSource = File.m_Path.f_Extract(FindPath.f_GetLen() + 1);
						if (CBackupManager::fs_IsValidBackupSource(BackupSource, nullptr, nullptr))
							BackupSources.f_Insert(BackupSource);
					}
					return BackupSources;
				}
			)
		;
	}

	TCVector<CStr> CBackupManagerServer::fp_EnumBackupSources()
	{
		TCVector<CStr> Sources;
		for (auto &Source : mp_BackupSources)
			Sources.f_Insert(mp_BackupSources.fs_GetKey(Source));

		return Sources;
	}

	TCActor<CBackupSource> const &CBackupManagerServer::fp_CreateBackupSource(CStr const &_Source)
	{
		auto &BackupSource = mp_BackupSources[_Source];

		if (!BackupSource)
			BackupSource = fg_Construct(fg_Construct("{}/Backups/{}"_f << mp_AppState.m_RootDirectory << _Source), "Backup Source {}"_f << _Source);

		return BackupSource;
	}

	TCActor<CBackupSource> const *CBackupManagerServer::fp_GetBackupSource(CStr const &_Source)
	{
		return mp_BackupSources.f_FindEqual(_Source);
	}

	CBackupSource::CBackupSource(CStr const &_Directory)
		: mp_Directory(_Directory)
		, mp_LatestDirectory("{}/Latest"_f << _Directory)
	{
		g_Dispatch / [this]
			{
				fp_Init();
			}
			> fg_DiscardResult()
		;
	}

	CBackupSource::~CBackupSource()
	{
	}

	TCFuture<void> CBackupSource::fp_Destroy()
	{
		for (auto &State : mp_CheckedOutStates)
			State.f_Cleanup();

		mp_CheckedOutStates.f_Clear();

		co_return {};
	}

	void CBackupSource::fp_Init()
	{
		try
		{
			// Clear out old checked out directories from old processes
			for (auto &File : CFile::fs_FindFiles(mp_Directory + "/CheckedOut_*", EFileAttrib_Directory))
			{
				try
				{
					CFile::fs_DeleteDirectoryRecursive(File);
				}
				catch (NException::CException const &_Exception)
				{
					DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to delete checked out directories: {}", _Exception);
				}
			}

			// Enumerate all snapshots
			mp_BackupInfo.m_Earliest = CTime::fs_EndOfTime();
			mp_BackupInfo.m_Latest = CTime::fs_StartOfTime();
			for (auto &Snapshot : CFile::fs_FindFiles(mp_Directory + "/Snapshots/*", EFileAttrib_Directory))
			{
				CTime Time;
				if (!CBackupManager::fs_IsValidBackupTime(CFile::fs_GetFile(Snapshot), &Time))
					continue;

				mp_BackupInfo.m_Snapshots.f_Insert(Time);
				mp_BackupInfo.m_Earliest = fg_Min(mp_BackupInfo.m_Earliest, Time);
				mp_BackupInfo.m_Latest = fg_Min(mp_BackupInfo.m_Earliest, Time);
			}
		}
		catch (NException::CException const &_Exception)
		{
			DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to clean up old checked out directories: {}", _Exception);
		}
	}

	void CBackupSource::CCheckedOutState::f_Cleanup()
	{
		try
		{
			CFile::fs_DeleteDirectoryRecursive(m_Directory);
		}
		catch (CExceptionFile const &_Exception)
		{
			DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to clean up checked out directory: {}", _Exception);
		}
	}

	TCFuture<CBackupManager::CBackupInfo> CBackupSource::f_GetInfo()
	{
		co_return mp_BackupInfo;
	}

	auto CBackupSource::f_CheckOutDirectory(CTime const &_Time) -> TCFuture<CCheckedOutDirectory>
	{
		CStr CheckedOutID = fg_RandomID(mp_CheckedOutStates);

		auto &State = mp_CheckedOutStates[CheckedOutID];
		State.m_Directory = "{}/CheckedOut_{}"_f << mp_Directory << CheckedOutID;

		CCheckedOutDirectory Result;
		Result.m_Directory = State.m_Directory;
		Result.m_Subscription = g_ActorSubscription / [this, CheckedOutID]() -> TCFuture<void>
			{
				auto *pState = mp_CheckedOutStates.f_FindEqual(CheckedOutID);
				if (!pState)
					co_return {};

				pState->f_Cleanup();

				mp_CheckedOutStates.f_Remove(pState);

				co_return {};
			}
		;

		try
		{
			CFile::CFindFilesOptions Options{mp_LatestDirectory + "/*", true};
			Options.m_AttribMask = EFileAttrib_File | EFileAttrib_Link;

			CStr BaseDirectory = State.m_Directory / "Current";
			CFile::fs_CreateDirectory(BaseDirectory);

			CStr ManifestFileName = "{}/Manifest.bin"_f << mp_Directory;
			if (!CFile::fs_FileExists(ManifestFileName))
				DMibError("Initial manifest does not exist");

			CFile::fs_CreateHardLink(ManifestFileName, State.m_Directory / "Manifest.bin");

			for (auto &File : CFile::fs_FindFiles(Options))
			{
				CStr RelativePath = CFile::fs_MakePathRelative(File.m_Path, mp_LatestDirectory);
				CStr DestinationPath = BaseDirectory / RelativePath;
				CFile::fs_CreateDirectory(CFile::fs_GetPath(DestinationPath));
				CFile::fs_CreateHardLink(File.m_Path, DestinationPath);
			}

			co_return fg_Move(Result);
		}
		catch (CException const &)
		{
			co_return fg_CurrentException();
		}
	}

	void CBackupSource::fp_SaveManifest()
	{
		CStr ManifestFileName = "{}/Manifest.bin"_f << mp_Directory;
		CStr TempFileName = "{}/TempFile"_f << mp_Directory;

		{
			TCBinaryStreamFile<> Stream;
			Stream.f_Open(ManifestFileName + ".tmp", EFileOpen_Write | EFileOpen_ShareAll, EFileAttrib_UserRead | EFileAttrib_UserWrite);
			Stream << mp_CurrentManifest;
		}

		if (CFile::fs_FileExists(ManifestFileName))
			CFile::fs_AtomicReplaceFile(ManifestFileName + ".tmp", ManifestFileName);
		else
			CFile::fs_RenameFile(ManifestFileName + ".tmp", ManifestFileName);
	}

	void CBackupSource::fp_CleanupOldBackups()
	{
		TCSet<CStr> ProtectedBackups;

		for (auto &Backup : mp_Backups)
			ProtectedBackups[CStr{"Sync_{}"_f << mp_Backups.fs_GetKey(Backup)}];

		for (auto &Directory : CFile::fs_FindFiles(mp_Directory / "Sync_*"))
		{
			if (ProtectedBackups.f_FindEqual(CFile::fs_GetFile(Directory)))
				continue;

			try
			{
				CFile::fs_DeleteDirectoryRecursive(Directory);
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to delete old sync directory: {}", _Exception);
			}
		}
	}

	auto CBackupSource::f_InitialCommit(CStr const &_BackupID, CStr const &_Directory, CDirectoryManifest &&_Manifest, CBackupManagerBackup::EInitialBackupFinishedFlag _FinishedFlags)
		-> TCFuture<CInitialCommitResult>
	{
		return TCFuture<CInitialCommitResult>::fs_RunProtected() / [&]() mutable
			{
				if (mp_Backups.f_FindEqual(_BackupID))
					DMibError("Backup with this ID has already done initial commit");

				CInitialCommitResult CommitResult;

				CStr TempFileName = "{}/TempFile"_f << mp_Directory;

				mp_CurrentBackupID = _BackupID;
				mp_CurrentManifest = fg_Move(_Manifest);

				CFile::fs_CreateDirectory(mp_Directory);

				for (auto &Backup : mp_Backups)
					Backup.m_AppendFiles.f_Clear();

				CFile::CFindFilesOptions Options{mp_LatestDirectory + "/*", true};
				Options.m_AttribMask = EFileAttrib_File | EFileAttrib_Directory | EFileAttrib_FindDirectoryLast;

				TCSet<CStr> ExtraFiles;
				TCSet<CStr> ExtraDirectories;
				TCVector<CStr> OldDirectories;

				for (auto &File : CFile::fs_FindFiles(Options))
				{
					auto RelativePath = CFile::fs_MakePathRelative(File.m_Path, mp_LatestDirectory);
					if (File.m_Attribs & EFileAttrib_File)
						ExtraFiles[RelativePath];
					else
					{
						ExtraDirectories[RelativePath];
						OldDirectories.f_Insert(RelativePath);
					}
				}

#ifdef DMibCloudBackupManagerDebug
				TCSet<CStr> InitialFiles;
#endif

				for (auto &File : mp_CurrentManifest.m_Files)
				{
					auto &FileName = mp_CurrentManifest.m_Files.fs_GetKey(File);
					if (File.f_IsDirectory())
					{
						ExtraDirectories.f_Remove(FileName);
						continue;
					}
					if (!File.f_IsFile())
						continue;

#ifdef DMibCloudBackupManagerDebug
					InitialFiles[FileName];
#endif
					ExtraFiles.f_Remove(FileName);

					CStr FullSourcePath = CFile::fs_AppendPath(_Directory, FileName);
					CStr FullDestinationPath = CFile::fs_AppendPath(mp_LatestDirectory, FileName);

					CFile::fs_CreateDirectory(CFile::fs_GetPath(FullDestinationPath));

					if (File.m_Flags & EDirectoryManifestSyncFlag_Append)
					{
						if (!CFile::fs_TryDuplicateFile(FullSourcePath, TempFileName))
							CFile::fs_CopyFile(FullSourcePath, TempFileName);
						FullSourcePath = TempFileName;
					}

					if (CFile::fs_FileExists(FullDestinationPath))
					{
						if (_FinishedFlags & CBackupManagerBackup::EInitialBackupFinishedFlag_ReturnChanges)
						{
							if (CFile::fs_GetUniqueIdentifier(FullSourcePath) != CFile::fs_GetUniqueIdentifier(FullDestinationPath))
								CommitResult.m_Result.m_UpdatedFiles.f_Insert(FileName);
						}

#ifdef DPlatformFamily_Windows
						for (mint i = 0; i < 200; ++i)
						{
							try
							{
								if (CFile::fs_FileExists(FullDestinationPath))
									CFile::fs_AtomicReplaceFile(FullSourcePath, FullDestinationPath);
								else
									CFile::fs_RenameFile(FullSourcePath, FullDestinationPath);
								break;
							}
							catch (CExceptionFile const &)
							{
								try
								{
									if (CFile::fs_FileExists(FullDestinationPath))
										CFile::fs_DeleteFile(FullDestinationPath);
									CFile::fs_RenameFile(FullSourcePath, FullDestinationPath);
									break;
								}
								catch (CExceptionFile const&)
								{
									if (i == 199)
										throw;
									NSys::fg_Thread_Sleep(0.01f);
								}
							}
						}
#else
						CFile::fs_AtomicReplaceFile(FullSourcePath, FullDestinationPath);
#endif
					}
					else
					{
						if (_FinishedFlags & CBackupManagerBackup::EInitialBackupFinishedFlag_ReturnChanges)
							CommitResult.m_Result.m_AddedFiles.f_Insert(FileName);
						CFile::fs_RenameFile(FullSourcePath, FullDestinationPath);
					}
				}

				for (auto &FileName : ExtraFiles)
				{
					if (_FinishedFlags & CBackupManagerBackup::EInitialBackupFinishedFlag_ReturnChanges)
						CommitResult.m_Result.m_RemovedFiles.f_Insert(FileName);
					CFile::fs_DeleteFile(CFile::fs_AppendPath(mp_LatestDirectory, FileName));
				}

				for (auto &FileName : OldDirectories)
				{
					if (!ExtraDirectories.f_FindEqual(FileName))
						continue;
					CFile::fs_DeleteDirectory(CFile::fs_AppendPath(mp_LatestDirectory, FileName));
				}

				DMibCloudBackupManagerDebugOut("*** Initial {vs}\n", InitialFiles);

				fp_SaveManifest();
				mp_LatestBackupID = mp_CurrentBackupID;

				auto &Backup = mp_Backups[_BackupID];
				Backup.m_SourceDirectory = _Directory;

				fp_CleanupOldBackups();

				CommitResult.m_Subscription = g_ActorSubscription / [this, _BackupID]
					{
						if (!mp_LatestBackupID.f_IsEmpty() && _BackupID != mp_LatestBackupID)
						{
							try
							{
								CStr Directory = "{}/Sync_{}"_f << mp_Directory << _BackupID;
								if (CFile::fs_FileExists(Directory))
									CFile::fs_DeleteDirectoryRecursive(Directory);
							}
							catch (CException const &_Exception)
							{
								(void)_Exception;
								DMibLogCategory(Mib/Cloud/BackupManager);
								DMibLog(Error, "Failed to delete sync directory: {}", _Exception);
							}
						}

						mp_Backups.f_Remove(_BackupID);
						if (mp_CurrentBackupID == _BackupID)
							mp_CurrentBackupID.f_Clear();
					}
				;

				return CommitResult;
			}
		;
	}

	TCFuture<void> CBackupSource::f_Commit(CStr const &_BackupID, CStr const &_File, CBackupManagerBackup::CManifestChange const &_ManifestChange)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestChangeValid(_File, _ManifestChange, ManifestError))
			co_return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _File << ManifestError);

		auto pBackup = mp_Backups.f_FindEqual(_BackupID);
		if (!pBackup)
			co_return DMibErrorInstance("Backup with this ID has not done initial commit");

		if (_BackupID != mp_CurrentBackupID)
			co_return {};

		auto &Backup = *pBackup;

		CStr SourceFile = Backup.m_SourceDirectory / _File;
		CStr DestinationFile = mp_LatestDirectory / _File;
		CStr TempFileName = "{}/TempFile"_f << mp_Directory;

		auto fApplyAppend = [&](CDirectoryManifestFile const &_File)
			{
				if (_File.m_Flags & EDirectoryManifestSyncFlag_Append)
				{
					// TODO: Replace with duplicate functionality
					CFile::fs_CopyFile(SourceFile, TempFileName);
					SourceFile = TempFileName;
				}
			}
		;

		try
		{
			TCSet<CStr> CheckDeleteDirectories;

			auto Cleanup = g_OnScopeExit / [&]
				{
					while (!CheckDeleteDirectories.f_IsEmpty())
					{
						auto ToCheck = fg_Move(CheckDeleteDirectories);
						for (auto &CheckDirectory : ToCheck)
						{
							if (!CheckDirectory.f_StartsWith(mp_LatestDirectory))
								continue;

							try
							{
								if (CFile::fs_FindFiles(CheckDirectory / "*").f_IsEmpty())
								{
									CFile::fs_DeleteDirectory(CheckDirectory);
									CheckDeleteDirectories[CFile::fs_GetPath(CheckDirectory)];
								}
							}
							catch (NException::CException const &_Exception)
							{
								[[maybe_unused]] auto &Exception = _Exception;
								DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to delete unused directory after commit: {}", Exception);
							}
						}
					}
				}
			;
			
			auto &Change = _ManifestChange;
			switch (Change.f_GetTypeID())
			{
			case CBackupInstance::EManifestChange_Add:
				{
					auto &SpecificChange = Change.f_Get<CBackupInstance::EManifestChange_Add>();

					DMibCloudBackupManagerDebugOut("*** Add {}\n", _File);

					auto pOriginalManifestFile = mp_CurrentManifest.m_Files.f_FindEqual(_File);
					if (pOriginalManifestFile)
						DMibError("Found unexpected original manifest file '{}' for add operation"_f << _File);

					if (SpecificChange.m_ManifestFile.f_IsFile())
					{
						fApplyAppend(SpecificChange.m_ManifestFile);

						fp_CloseFiles(DestinationFile);

						if (CFile::fs_FileExists(DestinationFile, EFileAttrib_Directory))
							CFile::fs_DeleteDirectoryRecursive(DestinationFile);
						else if (CFile::fs_FileExists(DestinationFile))
							DMibError("Destination file '{}' exists when it shouldn't for add operation"_f << DestinationFile);

						CFile::fs_CreateDirectory(CFile::fs_GetPath(DestinationFile));
						CFile::fs_RenameFile(SourceFile, DestinationFile);
					}

					CBackupManagerBackup::fs_ApplyManifestChange(mp_CurrentManifest, _File, Change);
					break;
				}
			case CBackupInstance::EManifestChange_Change:
				{
					DMibCloudBackupManagerDebugOut("*** Change {}\n", _File);

					auto &SpecificChange = Change.f_Get<CBackupInstance::EManifestChange_Change>();

					auto pOriginalManifestFile = mp_CurrentManifest.m_Files.f_FindEqual(_File);
					if (!pOriginalManifestFile)
						DMibError("Could not find original manifest file '{}' for change operation"_f << _File);

					if (SpecificChange.m_ManifestFile.f_IsFile())
					{
						fApplyAppend(SpecificChange.m_ManifestFile);

						fp_CloseFiles(DestinationFile);

						if (pOriginalManifestFile->f_IsFile())
						{
							if (!CFile::fs_FileExists(DestinationFile))
								DMibError("Destination file '{}' does not exist for change operation"_f << DestinationFile);
							CFile::fs_AtomicReplaceFile(SourceFile, DestinationFile);
						}
						else
						{
							if (CFile::fs_FileExists(DestinationFile, EFileAttrib_Directory))
								CFile::fs_DeleteDirectoryRecursive(DestinationFile);
							else if (CFile::fs_FileExists(DestinationFile))
								DMibError("Destination file '{}' exists when it shouldn't for change operation"_f << DestinationFile);

							CFile::fs_CreateDirectory(CFile::fs_GetPath(DestinationFile));
							CFile::fs_RenameFile(SourceFile, DestinationFile);
						}
					}
					else if (pOriginalManifestFile->f_IsFile())
					{
						if (!CFile::fs_FileExists(DestinationFile))
							DMibError("Destination file '{}' does not exist for change operation"_f << DestinationFile);

						fp_CloseFiles(DestinationFile);

						CFile::fs_DeleteFile(DestinationFile);
						CheckDeleteDirectories[CFile::fs_GetPath(DestinationFile)];
					}

					CBackupManagerBackup::fs_ApplyManifestChange(mp_CurrentManifest, _File, Change);
					break;
				}
			case CBackupInstance::EManifestChange_Remove:
				{
					auto pOriginalManifestFile = mp_CurrentManifest.m_Files.f_FindEqual(_File);
					if (!pOriginalManifestFile)
						DMibError("Could not find original manifest file '{}' for remove operation"_f << _File);

					if (pOriginalManifestFile->f_IsFile())
					{
						if (!CFile::fs_FileExists(DestinationFile))
							DMibError("Source file '{}' does not exists for remove operation"_f << DestinationFile);

						DMibCloudBackupManagerDebugOut("*** Remove {}\n", _File);

						fp_CloseFiles(DestinationFile);
						CFile::fs_DeleteFile(DestinationFile);
						CheckDeleteDirectories[CFile::fs_GetPath(DestinationFile)];
					}
					else
						DMibCloudBackupManagerDebugOut("*** Remove {} (NOT FILE)\n", _File);

					CBackupManagerBackup::fs_ApplyManifestChange(mp_CurrentManifest, _File, Change);
					break;
				}
			case CBackupInstance::EManifestChange_Rename:
				{
					auto &SpecificChange = Change.f_Get<CBackupInstance::EManifestChange_Rename>();

					CStr OriginalFile = CFile::fs_AppendPath(mp_LatestDirectory, SpecificChange.m_FromFileName);

					auto pOriginalManifestFile = mp_CurrentManifest.m_Files.f_FindEqual(SpecificChange.m_FromFileName);
					if (!pOriginalManifestFile)
						DMibError("Could not find original manifest file '{}' for rename operation"_f << SpecificChange.m_FromFileName);

					if (SpecificChange.m_ManifestFile.f_IsFile())
					{
						if (pOriginalManifestFile->f_IsFile())
						{
							if (!CFile::fs_FileExists(OriginalFile))
								DMibError("Source file '{}' does not exist for rename operation"_f << OriginalFile);

							fp_CloseFiles(OriginalFile);
							fp_CloseFiles(DestinationFile);

							if (CFile::fs_FileExists(DestinationFile))
								CFile::fs_AtomicReplaceFile(SourceFile, DestinationFile);
							else
							{
								CFile::fs_CreateDirectory(CFile::fs_GetPath(DestinationFile));
								CFile::fs_RenameFile(OriginalFile, DestinationFile);
							}
							CFile::fs_CreateDirectory(CFile::fs_GetPath(DestinationFile));
							CheckDeleteDirectories[CFile::fs_GetPath(OriginalFile)];
							DMibCloudBackupManagerDebugOut("*** Rename {} -> {}\n", SpecificChange.m_FromFileName, _File);
						}
						else
						{
							if (auto pOld = mp_CurrentManifest.m_Files.f_FindEqual(SpecificChange.m_FromFileName))
							{
								auto Old = *pOld;
								mp_CurrentManifest.m_Files.f_Remove(SpecificChange.m_FromFileName);
								mp_CurrentManifest.m_Files[_File] = fg_Move(Old);
							}
						}
					}
					else if (pOriginalManifestFile->f_IsFile())
					{
						DMibCloudBackupManagerDebugOut("*** Rename {} -> {} (DELTED)\n", SpecificChange.m_FromFileName, _File);

						fp_CloseFiles(OriginalFile);

						if (CFile::fs_FileExists(OriginalFile))
							CFile::fs_DeleteFile(OriginalFile);
						CheckDeleteDirectories[CFile::fs_GetPath(OriginalFile)];
					}
					else
						DMibCloudBackupManagerDebugOut("*** Rename {} -> {} (NONE FILES)\n", SpecificChange.m_FromFileName, _File);
					CBackupManagerBackup::fs_ApplyManifestChange(mp_CurrentManifest, _File, Change);
					break;
				}
			}
			try
			{
				fp_SaveManifest();
			}
			catch (NException::CException const &_Exception)
			{
				DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to save manifest after commit: {}", _Exception);
				throw;
			}

			co_return {};
		}
		catch (CException const &)
		{
			co_return fg_CurrentException();
		}
	}

	void CBackupSource::fp_CloseFiles(CStr const &_FileName)
	{
		for (auto &Backup : mp_Backups)
			Backup.m_AppendFiles.f_Remove(_FileName);
	}

	TCFuture<void> CBackupSource::f_CommitAppend
		(
		 	CStr const &_BackupID
		 	, CStr const &_File
		 	, uint64 _Position
		 	, CSecureByteVector &&_Data
		 	, CBackupManagerBackup::CManifestChange &&_ManifestChange
		)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestChangeValid(_File, _ManifestChange, ManifestError))
			co_return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _File << ManifestError);

		auto pBackup = mp_Backups.f_FindEqual(_BackupID);
		if (!pBackup)
			co_return DMibErrorInstance("Backup with this ID has not done initial commit");

		if (_BackupID != mp_CurrentBackupID)
			co_return {};

		auto &Backup = *pBackup;

		CStr DestinationFile = CFile::fs_AppendPath(mp_LatestDirectory, _File);

		auto pOriginalManifestFile = mp_CurrentManifest.m_Files.f_FindEqual(_File);
		if (!pOriginalManifestFile)
			co_return DMibErrorInstance("Could not find original manifest file '{}' for append operation"_f << _File);

		if (!pOriginalManifestFile->f_IsFile())
			co_return DMibErrorInstance("Original manifest file '{}' is not a file in append operation"_f << _File);

		try
		{
			auto Cleanup = g_OnScopeExit / [&]
				{
					bool bInException = NException::fg_UncaughtExceptions();

					try
					{
						fp_SaveManifest();
					}
					catch (NException::CException const &_Exception)
					{
						[[maybe_unused]] auto &Exception = _Exception;
						DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to save manifest after append commit: {}", Exception);
						if (!bInException)
							throw;
					}
				}
			;

			switch (_ManifestChange.f_GetTypeID())
			{
			case CBackupInstance::EManifestChange_Change:
				{
					auto &SpecificChange = _ManifestChange.f_Get<CBackupInstance::EManifestChange_Change>();

					if (!SpecificChange.m_ManifestFile.f_IsFile())
						DMibError("Manifest file '{}' is not a file in append operation"_f << _File);

					if (!CFile::fs_FileExists(DestinationFile))
						DMibError("Destination file '{}' does not exist for append operation"_f << DestinationFile);

					auto &File = Backup.m_AppendFiles[DestinationFile];

					if (!File.f_IsValid())
						File.f_Open(DestinationFile, EFileOpen_Write | EFileOpen_ShareAll | EFileOpen_DontTruncate | EFileOpen_NoLocalCache);

					File.f_SetPosition(_Position);
					File.f_Write(_Data.f_GetArray(), _Data.f_GetLen());
					File.f_Flush(false);

					DMibCloudBackupManagerDebugOut("*** Append {}\n", _File);
					CBackupManagerBackup::fs_ApplyManifestChange(mp_CurrentManifest, _File, _ManifestChange);

					break;
				}
			default:
				{
					DMibError("Non change manifest in append operation");
					break;
				}
			}
			co_return {};
		}
		catch (CException const &)
		{
			co_return fg_CurrentException();
		}
	}
}
