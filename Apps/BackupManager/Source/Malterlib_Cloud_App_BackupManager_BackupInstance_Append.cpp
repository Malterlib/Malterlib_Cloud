// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	TCFuture<void> CBackupInstance::f_AppendData(CStr _FileName, CAppendData _Data)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestFileValid(_FileName, _Data.m_ManifestFile, ManifestError))
			co_return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _FileName << ManifestError);

		auto &Internal = *mp_pInternal;

		if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
			co_return fg_Move(pException);

		if (!_Data.m_ManifestFile.f_IsFile())
			co_return DMibErrorInstance("Cannot append non-file: {}"_f << _FileName);

		DMibCloudBackupManagerDebugOut("--- Append {}\n", _FileName);

		TCPromiseFuturePair<void> Promise;

		auto Subscription = co_await Internal.f_SequenceSyncs(_FileName);
		CDirectoryManifestFile *pManifestFile;

		if (auto pException = Internal.f_CheckFileName(_FileName, &pManifestFile))
			co_return fg_Move(pException);

		auto fDigestFailed = [&]
			{
				Internal.m_AppendStates.f_Remove(_FileName);
			}
		;

		if (pManifestFile->m_Digest && *pManifestFile->m_Digest != _Data.m_PreviousDigest)
		{
			fDigestFailed();
			co_return DMibErrorInstanceBackupManagerHashMismatch("Previous digest does not match");
		}

		CManifestChange_Change ManifestChange;

		co_await
			(
				[&]() -> TCUnsafeFuture<void> // Safe here bacuse we don't suspend
				{
					auto CaptureScope = co_await g_CaptureExceptions;

					auto &ManifestFile = *pManifestFile;

					auto pAppendState = Internal.m_AppendStates.f_FindEqual(_FileName);
					if (!pAppendState)
					{
						CInternal::CAppendFileState State;

						CStr FileName = Internal.f_GetCurrentPath(_FileName);
						CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
						State.m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll | EFileOpen_NoLocalCache);

						CFileIoTempBuffer Buffer;

						for (uint64 ToDigest = ManifestFile.m_Length; ToDigest; )
						{
							auto BufferResult = Buffer.f_UseBuffer(ToDigest);

							State.m_File.f_Read(BufferResult.m_pBuffer, BufferResult.m_nBytes);
							State.m_Hash.f_AddData(BufferResult.m_pBuffer, BufferResult.m_nBytes);

							ToDigest -= BufferResult.m_nBytes;
						}

						if (ManifestFile.m_Digest && State.m_Hash.f_GetDigest() != *ManifestFile.m_Digest)
						{
							fDigestFailed();
							DMibErrorInstanceBackupManagerHashMismatch("Manifest digest does not match digest on file");
						}

						pAppendState = &(Internal.m_AppendStates[_FileName] = fg_Move(State));
					}

					auto &AppendState = *pAppendState;

					if (AppendState.m_File.f_GetPosition() != _Data.m_Position)
						DError("Trying to append data to non-consecutive file position");

					ManifestFile = _Data.m_ManifestFile;

					auto OldState = AppendState.m_Hash;
					AppendState.m_Hash.f_AddData(_Data.m_Data.f_GetArray(), _Data.m_Data.f_GetLen());

					auto NewDigest = AppendState.m_Hash.f_GetDigest();

					if (_Data.m_ManifestFile.m_Digest && NewDigest != *_Data.m_ManifestFile.m_Digest)
					{
						AppendState.m_Hash = OldState;
						fDigestFailed();
						DMibErrorInstanceBackupManagerHashMismatch("Sent data results in a digest that does not match");
					}

					AppendState.m_File.f_SetPosition(_Data.m_Position);
					AppendState.m_File.f_Write(_Data.m_Data.f_GetArray(), _Data.m_Data.f_GetLen());

					ManifestChange.m_ManifestFile = _Data.m_ManifestFile;

					CBackupManagerBackup::fs_ApplyManifestChange(Internal.m_Manifest, _FileName, ManifestChange);

					DMibLogWithCategory
						(
							Mib/Cloud/BackupManager
							, Debug
							, "Appending data for file '{}':   at {}   {} bytes"
							, _FileName
							, _Data.m_Position
							, _Data.m_Data.f_GetLen()
						)
					;

					co_return {};
				}
				()
			)
		;

		Internal.m_Manifest.m_Files[_FileName] = _Data.m_ManifestFile;

		if (!Internal.m_bInitialBackupFinished)
			co_return {};

		co_await Internal.m_BackupSource(&CBackupSource::f_CommitAppend, Internal.m_ID, _FileName, _Data.m_Position, fg_Move(_Data.m_Data), ManifestChange);

		co_return {};
	}
}
