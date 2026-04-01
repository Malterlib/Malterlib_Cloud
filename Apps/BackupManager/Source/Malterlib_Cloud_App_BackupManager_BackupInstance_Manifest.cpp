// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

#include <Mib/Cryptography/RandomID>

namespace NMib::NCloud::NBackupManager
{
	TCFuture<TCActorSubscriptionWithID<>> CBackupInstance::f_StartManifestRSync
		(
			FRunRSyncProtocol _fRunProtocol
			, uint64 _ManifestSize
			, NCryptography::CHashDigest_SHA256 _ExpectedDigest
		)
	{
		auto ProtocolVersion = fg_GetCallingHostInfo().f_GetProtocolVersion();

		auto &Internal = *mp_pInternal;
		if (Internal.m_bManifestSyncStarted)
			co_return DMibErrorInstance("Manifest rsync already started");

		Internal.m_bManifestSyncStarted = true;

		CStr FileName = CFile::fs_AppendPath(Internal.m_BackupDirectory, "Manifest.bin");
		CStr OldFileName = CFile::fs_AppendPath(Internal.m_RootBackupDirectory, "Manifest.bin");
		CStr TempFileName = fg_Format("{}.{}.tmp", FileName, fg_FastRandomID());

		co_return co_await Internal.f_StartRSyncShared
			(
				fg_Move(_fRunProtocol)
				, FileName
				, OldFileName
				, TempFileName
				, "../Manifest.bin"
				, _ManifestSize
				, EDirectoryManifestSyncFlag_None
				, &Internal.m_ManifestRSyncID
				, [this](TCAsyncResult<void> const &_Result) -> TCFuture<void>
				{
					auto &Internal = *mp_pInternal;
					if (_Result)
						Internal.m_bManifestSyncDone = true;
					return _Result;
				}
				, _ExpectedDigest
				, ProtocolVersion
			)
		;
	}

	TCFuture<void> CBackupInstance::f_ManifestChange(CStr _FileName, CManifestChange _Change)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestChangeValid(_FileName, _Change, ManifestError))
			co_return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _FileName << ManifestError);

		auto &Internal = *mp_pInternal;

		TCSet<CStr> ManifestFiles;
		for (auto &File : Internal.m_Manifest.m_Files)
			ManifestFiles[Internal.m_Manifest.m_Files.fs_GetKey(File)];

		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					co_return fg_Move(pException);

				auto &SpecificChange = _Change.f_Get<EManifestChange_Add>();
				if (SpecificChange.m_ManifestFile.f_IsFile())
					co_return DMibErrorInstance("Add not valid for a file change, use rsync interface");

				DMibCloudBackupManagerDebugOut("--- Add {}\n", _FileName);

				auto Subscription = co_await Internal.f_SequenceSyncs(_FileName);
				DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Add manifest: {}", _FileName);

				if (Internal.m_Manifest.m_Files.f_FindEqual(_FileName))
					co_return DMibErrorInstance("File already exists in manifest: {}"_f << _FileName);

				Internal.m_AppendStates.f_Remove(_FileName);

				co_await Internal.f_CommitManifestChange(_FileName, _Change, "add");

				co_return {};
			}
		case EManifestChange_Change:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					co_return fg_Move(pException);

				auto &SpecificChange = _Change.f_Get<EManifestChange_Change>();

				if (SpecificChange.m_ManifestFile.f_IsFile())
					co_return DMibErrorInstance("Change not valid for a file change, use append or rsync interface");

				DMibCloudBackupManagerDebugOut("--- Change {}\n", _FileName);

				auto Subscription = co_await Internal.f_SequenceSyncs(_FileName);

				DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Change manifest: {}", _FileName);

				auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(_FileName);

				if (!pManifestFile)
					co_return DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName);

				Internal.m_AppendStates.f_Remove(_FileName);
				co_await Internal.f_CommitManifestChange(_FileName, _Change, "change");

				co_return {};
			}
		case EManifestChange_Remove:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					co_return fg_Move(pException);

				DMibCloudBackupManagerDebugOut("--- Remove {}\n", _FileName);

				auto PendingCleanup = Internal.f_FilePending(_FileName);

				TCPromiseFuturePair<void> Promise;
				auto Subscription = co_await Internal.f_SequenceSyncs(_FileName);

				DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Remove manifest: {}", _FileName);

				Internal.m_AppendStates.f_Remove(_FileName);

				auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(_FileName);

				if (!pManifestFile)
					co_return DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName);

				CStr AbsolutePath = Internal.f_GetCurrentPath(_FileName);

				if (pManifestFile->f_IsFile())
				{
					try
					{
						if (CFile::fs_FileExists(AbsolutePath))
							CFile::fs_DeleteFile(AbsolutePath);
					}
					catch (CExceptionFile const &_Exception)
					{
						[[maybe_unused]] auto &Exception = _Exception;
						DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to apply file remove locally: {}", Exception);
					}
				}

				co_await Internal.f_CommitManifestChange(_FileName, _Change, "remove");

				co_return {};
			}
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();

				if (auto pException = Internal.f_CheckFileName(Change.m_FromFileName, nullptr))
					co_return fg_Move(pException);

				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					co_return fg_Move(pException);

				auto PendingCleanup = Internal.f_FilePending(_FileName);
				auto PendingCleanup2 = Internal.f_FilePending(Change.m_FromFileName);

				DMibCloudBackupManagerDebugOut("--- Rename {} -> {}\n", Change.m_FromFileName, _FileName);

				TCPromiseFuturePair<void> Promise;
				auto Subscription = co_await Internal.f_SequenceMultipleSyncs({_FileName, Change.m_FromFileName});

				DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Rename manifest: {} -> {}", Change.m_FromFileName, _FileName);

				Internal.m_AppendStates.f_Remove(Change.m_FromFileName);
				Internal.m_AppendStates.f_Remove(_FileName);

				auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(Change.m_FromFileName);

				if (!pManifestFile)
					co_return DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName);

				CStr AbsoluteFrom = Internal.f_GetCurrentPath(Change.m_FromFileName);
				CStr AbsoluteTo = Internal.f_GetCurrentPath(_FileName);

				(void)PendingCleanup;
				(void)PendingCleanup2;

				CStr AbsolutePath = Internal.f_GetCurrentPath(_FileName);

				DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Remove manifest: {}", _FileName);

				if (pManifestFile->f_IsFile())
				{
					try
					{
						if (CFile::fs_FileExists(AbsoluteFrom))
						{
							CFile::fs_CreateDirectory(CFile::fs_GetPath(AbsoluteTo));
							CFile::fs_AtomicReplaceFile(AbsoluteFrom, AbsoluteTo);
						}
					}
					catch (CExceptionFile const &_Exception)
					{
						[[maybe_unused]] auto &Exception = _Exception;
						DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to apply file rename locally: {}", Exception);
					}
				}

				co_await Internal.f_CommitManifestChange(_FileName, _Change, "rename");

				co_return {};
			}
		}

		DNeverGetHere;

		co_return DMibErrorInstance("Internal error");
	}
}
