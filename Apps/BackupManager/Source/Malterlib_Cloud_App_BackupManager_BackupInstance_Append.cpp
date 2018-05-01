
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

namespace NMib::NCloud::NBackupManager
{
	TCContinuation<void> CBackupInstance::f_AppendData(CStr const &_FileName, CAppendData &&_Data)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestFileValid(_FileName, _Data.m_ManifestFile, ManifestError))
			return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _FileName << ManifestError);

		auto &Internal = *mp_pInternal;

		if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
			return fg_Move(pException);

		if (!_Data.m_ManifestFile.f_IsFile())
			return DMibErrorInstance("Cannot append non-file: {}"_f << _FileName);

		TCContinuation<void> Continuation;
		DMibCloudBackupManagerDebugOut("--- Append {}\n", _FileName);

		Internal.f_SequenceSyncs
			(
			 	_FileName
				, [=, Data = fg_Move(_Data)](COnScopeExitShared &&_pCleanup) mutable
				{
					auto &Internal = *mp_pInternal;

					CDirectoryManifestFile *pManifestFile;

					if (auto pException = Internal.f_CheckFileName(_FileName, &pManifestFile))
						return Continuation.f_SetException(pException);

					auto fDigestFailed = [&]
						{
							Internal.m_AppendStates.f_Remove(_FileName);
						}
					;

					if (pManifestFile->m_Digest != Data.m_PreviousDigest)
					{
						fDigestFailed();
						return Continuation.f_SetException(DMibErrorInstanceBackupManagerHashMismatch("Previous digest does not match"));
					}

					CManifestChange_Change ManifestChange;

					auto Result = TCContinuation<void>::fs_RunProtected<CException>()
						> [&]()
						{
							auto &ManifestFile = *pManifestFile;

							auto pAppendState = Internal.m_AppendStates.f_FindEqual(_FileName);
							if (!pAppendState)
							{
								CInternal::CAppendFileState State;

								CStr FileName = Internal.f_GetCurrentPath(_FileName);
								CFile::fs_CreateDirectory(CFile::fs_GetPath(FileName));
								State.m_File.f_Open(FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll | EFileOpen_NoLocalCache);

								uint8 Buffer[32768];

								for (uint64 ToDigest = ManifestFile.m_Length; ToDigest; )
								{
									uint64 ThisTime = fg_Min(ToDigest, 32768);

									State.m_File.f_Read(Buffer, ThisTime);
									State.m_Hash.f_AddData(Buffer, ThisTime);

									ToDigest -= ThisTime;
								}

								if (State.m_Hash.f_GetDigest() != ManifestFile.m_Digest)
								{
									fDigestFailed();
									DMibErrorInstanceBackupManagerHashMismatch("Manifest digest does not match digest on file");
								}

								pAppendState = &(Internal.m_AppendStates[_FileName] = fg_Move(State));
							}

							auto &AppendState = *pAppendState;

							if (AppendState.m_File.f_GetPosition() != Data.m_Position)
								DError("Trying to append data to non-consecutive file position");

							ManifestFile = Data.m_ManifestFile;

							auto OldState = AppendState.m_Hash;
							AppendState.m_Hash.f_AddData(Data.m_Data.f_GetArray(), Data.m_Data.f_GetLen());

							auto NewDigest = AppendState.m_Hash.f_GetDigest();

							if (NewDigest != Data.m_ManifestFile.m_Digest)
							{
								AppendState.m_Hash = OldState;
								fDigestFailed();
								DMibErrorInstanceBackupManagerHashMismatch("Sent data results in a digest that does not match");
							}

							AppendState.m_File.f_SetPosition(Data.m_Position);
							AppendState.m_File.f_Write(Data.m_Data.f_GetArray(), Data.m_Data.f_GetLen());

							ManifestChange.m_ManifestFile = Data.m_ManifestFile;

							CBackupManagerBackup::fs_ApplyManifestChange(Internal.m_Manifest, _FileName, ManifestChange);

							DMibLogWithCategory
								(
									Mib/Cloud/BackupManager
									, Debug
									, "Appending data for file '{}':   at {}   {} bytes"
									, _FileName
									, Data.m_Position
									, Data.m_Data.f_GetLen()
								)
							;
						}
					;

					if (!Result)
					{
						Continuation.f_SetResult(Result);
						return;
					}

					Internal.m_Manifest.m_Files[_FileName] = Data.m_ManifestFile;

					if (!Internal.m_bInitialBackupFinished)
					{
						Continuation.f_SetResult();
						return;
					}

					Internal.m_BackupSource(&CBackupSource::f_CommitAppend, Internal.m_ID, _FileName, Data.m_Position, fg_Move(Data.m_Data), ManifestChange) > Continuation;
				}
			)
		;

		return Continuation;
	}
}
