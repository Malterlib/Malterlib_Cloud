
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

#include <Mib/Cryptography/RandomID>

namespace NMib::NCloud::NBackupManager
{
	TCFuture<TCActorSubscriptionWithID<>> CBackupInstance::f_StartManifestRSync
		(
			FRunRSyncProtocol &&_fRunProtocol
			, uint64 _ManifestSize
		 	, NCryptography::CHashDigest_SHA256 const &_ExpectedDigest
		)
	{
		auto ProtocolVersion = fg_GetCallingHostInfo().f_GetProtocolVersion();

		TCPromise<TCActorSubscriptionWithID<>> Promise;

		auto &Internal = *mp_pInternal;
		if (Internal.m_bManifestSyncStarted)
			return Promise <<= DMibErrorInstance("Manifest rsync already started");

		Internal.m_bManifestSyncStarted = true;

		CStr FileName = CFile::fs_AppendPath(Internal.m_BackupDirectory, "Manifest.bin");
		CStr OldFileName = CFile::fs_AppendPath(Internal.m_RootBackupDirectory, "Manifest.bin");
		CStr TempFileName = fg_Format("{}.{}.tmp", FileName, fg_RandomID());

		return Promise <<= fg_CallSafe
			(
			 	Internal
			 	, &CInternal::f_StartRSyncShared
				, fg_Move(_fRunProtocol)
				, FileName
				, OldFileName
				, TempFileName
				, "../Manifest.bin"
				, _ManifestSize
				, EDirectoryManifestSyncFlag_None
				, &Internal.m_ManifestRSyncID
				, [this](TCAsyncResult<void> const &_Result) -> TCFuture<void>
				{
					TCPromise<void> Promise;

					auto &Internal = *mp_pInternal;
					if (_Result)
						Internal.m_bManifestSyncDone = true;
					return Promise <<= _Result;
				}
			 	, _ExpectedDigest
				, ProtocolVersion
			)
		;
	}

	TCFuture<void> CBackupInstance::f_ManifestChange(CStr const &_FileName, CManifestChange const &_Change)
	{
		TCPromise<void> Promise;

		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestChangeValid(_FileName, _Change, ManifestError))
			return Promise <<= DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _FileName << ManifestError);

		auto &Internal = *mp_pInternal;

		TCSet<CStr> ManifestFiles;
		for (auto &File : Internal.m_Manifest.m_Files)
			ManifestFiles[Internal.m_Manifest.m_Files.fs_GetKey(File)];

		switch (_Change.f_GetTypeID())
		{
		case EManifestChange_Add:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return Promise <<= fg_Move(pException);

				auto &SpecificChange = _Change.f_Get<EManifestChange_Add>();
				if (SpecificChange.m_ManifestFile.f_IsFile())
					return Promise <<= DMibErrorInstance("Add not valid for a file change, use rsync interface");

				DMibCloudBackupManagerDebugOut("--- Add {}\n", _FileName);

				Internal.f_SequenceSyncs
					(
					 	_FileName
					 	, [=](COnScopeExitShared &&_pCleanup)
					 	{
							auto &Internal = *mp_pInternal;
							DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Add manifest: {}", _FileName);

							if (Internal.m_Manifest.m_Files.f_FindEqual(_FileName))
								return Promise.f_SetException(DMibErrorInstance("File already exists in manifest: {}"_f << _FileName));

							Internal.m_AppendStates.f_Remove(_FileName);

							fg_CallSafe(Internal, &CInternal::f_CommitManifestChange, _FileName, _Change, "add") > Promise;
						}
					)
				;
				return Promise.f_MoveFuture();
			}
		case EManifestChange_Change:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return Promise <<= fg_Move(pException);

				auto &SpecificChange = _Change.f_Get<EManifestChange_Change>();

				if (SpecificChange.m_ManifestFile.f_IsFile())
					return Promise <<= DMibErrorInstance("Change not valid for a file change, use append or rsync interface");

				DMibCloudBackupManagerDebugOut("--- Change {}\n", _FileName);

				Internal.f_SequenceSyncs
					(
						_FileName
						, [=](COnScopeExitShared &&_pCleanup)
						{
							auto &Internal = *mp_pInternal;
							DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Change manifest: {}", _FileName);

							auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(_FileName);

							if (!pManifestFile)
								return Promise.f_SetException(DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName));

							Internal.m_AppendStates.f_Remove(_FileName);
							fg_CallSafe(Internal, &CInternal::f_CommitManifestChange, _FileName, _Change, "change") > Promise;
						}
					)
				;
				return Promise.f_MoveFuture();
			}
		case EManifestChange_Remove:
			{
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return Promise <<= fg_Move(pException);

				DMibCloudBackupManagerDebugOut("--- Remove {}\n", _FileName);

				auto PendingCleanup = Internal.f_FilePending(_FileName);

				Internal.f_SequenceSyncs
					(
					 	_FileName
					 	, [Promise, _FileName, this, PendingCleanup, _Change](COnScopeExitShared &&_pCleanup)
					 	{
							auto &Internal = *mp_pInternal;
							DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Remove manifest: {}", _FileName);

							Internal.m_AppendStates.f_Remove(_FileName);

							auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(_FileName);

							if (!pManifestFile)
								return Promise.f_SetException(DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName));

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

							fg_CallSafe(Internal, &CInternal::f_CommitManifestChange, _FileName, _Change, "remove") > Promise;
						}
					)
				;
				return Promise.f_MoveFuture();
			}
		case EManifestChange_Rename:
			{
				auto &Change = _Change.f_Get<EManifestChange_Rename>();

				if (auto pException = Internal.f_CheckFileName(Change.m_FromFileName, nullptr))
					return Promise <<= fg_Move(pException);
				if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
					return Promise <<= fg_Move(pException);

				auto PendingCleanup = Internal.f_FilePending(_FileName);
				auto PendingCleanup2 = Internal.f_FilePending(Change.m_FromFileName);

				DMibCloudBackupManagerDebugOut("--- Rename {} -> {}\n", Change.m_FromFileName, _FileName);

				Internal.f_SequenceMultipleSyncs
					(
					 	[=](COnScopeExitShared &&_pCleanup)
					 	{
							auto &Internal = *mp_pInternal;
							DMibLogWithCategory(Mib/Cloud/BackupManager, DebugVerbose1, "Rename manifest: {} -> {}", Change.m_FromFileName, _FileName);

							Internal.m_AppendStates.f_Remove(Change.m_FromFileName);
							Internal.m_AppendStates.f_Remove(_FileName);

							auto *pManifestFile = Internal.m_Manifest.m_Files.f_FindEqual(Change.m_FromFileName);

							if (!pManifestFile)
								return Promise.f_SetException(DMibErrorInstance("File does not exists in manifest: {}"_f << _FileName));

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
										if (CFile::fs_FileExists(AbsoluteTo))
											CFile::fs_AtomicReplaceFile(AbsoluteFrom, AbsoluteTo);
										else
											CFile::fs_RenameFile(AbsoluteFrom, AbsoluteTo);
									}
								}
								catch (CExceptionFile const &_Exception)
								{
									[[maybe_unused]] auto &Exception = _Exception;
									DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to apply file rename locally: {}", Exception);
								}
							}

							fg_CallSafe(Internal, &CInternal::f_CommitManifestChange, _FileName, _Change, "rename") > Promise;
						}
					 	, {_FileName, Change.m_FromFileName}
					)
				;
				return Promise.f_MoveFuture();
			}
		}

		DNeverGetHere;

		return Promise.f_MoveFuture();
	}
}
