
#include "Malterlib_Cloud_App_BackupManager_BackupInstance.h"
#include "Malterlib_Cloud_App_BackupManager_BackupInstance_Internal.h"

#include <Mib/Cryptography/RandomID>

namespace NMib::NCloud::NBackupManager
{
	void CBackupInstance::CInternal::f_RunRSyncProtocol(CRSyncContext &_Context, CSecureByteVector &&_ServerPacket)
	{
		_Context.m_BytesTransferredIn += _ServerPacket.f_GetLen();

		bool bWantOneMoreProcess = true;
		bool bDone = false;
		while (bWantOneMoreProcess)
		{
			CSecureByteVector ToSendToServer;

			try
			{
				if (_Context.m_pClient->f_ProcessPacket(_ServerPacket, ToSendToServer, bWantOneMoreProcess))
					bDone = true;
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Exception running RSync protocol: {}", _Exception.f_GetErrorStr());
				m_RSyncContexts.f_Remove(_Context.f_GetSyncID());
				return;
			}

			_ServerPacket.f_Clear();
			if (!ToSendToServer.f_IsEmpty())
			{
				_Context.m_BytesTransferredOut += ToSendToServer.f_GetLen();

				_Context.m_fRunProtocol(fg_Move(ToSendToServer)) > [this, SyncID = _Context.f_GetSyncID()](TCAsyncResult<CSecureByteVector> &&_ServerPacket)
					{
						auto pContext = m_RSyncContexts.f_FindEqual(SyncID);
						if (!pContext)
							return;

						if (!_ServerPacket)
						{
							DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed run RSync protocol: {}", _ServerPacket.f_GetExceptionStr());
							m_RSyncContexts.f_Remove(SyncID);
							return;
						}

						f_RunRSyncProtocol(*pContext, fg_Move(*_ServerPacket));
					}
				;
			}
		}

		if (bDone)
		{
			try
			{
				auto ActualDigest = CFile::fs_GetFileChecksum_SHA256(_Context.m_AbsoluteFileName);
				if (_Context.m_ExpectedDigest != ActualDigest)
					_Context.m_bFailedHash = true;
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Exception running RSync protocol: {}", _Exception.f_GetErrorStr());
				m_RSyncContexts.f_Remove(_Context.f_GetSyncID());
				return;
			}

			_Context.m_fRunProtocol.f_Destroy() >
				[
					RelativeFileName = _Context.m_RelativeFileName
					, BytesTransferredIn = _Context.m_BytesTransferredIn
					, BytesTransferredOut = _Context.m_BytesTransferredOut
					, FileLength = _Context.m_FileLength
				]
				(TCAsyncResult<void> &&_Result) mutable
				{
					(void)RelativeFileName;
					(void)BytesTransferredIn;
					(void)BytesTransferredOut;
					(void)FileLength;

					[[maybe_unused]] uint64 BytesTransferred = BytesTransferredIn + BytesTransferredOut;
					if (BytesTransferred == 0)
						BytesTransferred = 1;

					DMibLogWithCategory
						(
							Mib/Cloud/BackupManager
							, Debug
							, "RSync protocol finished for file '{}':   {} incoming bytes   {} outgoing bytes   {fe1} speedup"
							, RelativeFileName
							, BytesTransferredIn
							, BytesTransferredOut
							, fp64(FileLength) / fp64(BytesTransferred)
						)
					;

					if (!_Result)
					{
						DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed destroy RSync protocol: {}", _Result.f_GetExceptionStr());
						return;
					}
				}
			;
		}
	}

	TCContinuation<TCActorSubscriptionWithID<>> CBackupInstance::CInternal::f_StartRSyncShared
		(
			FRunRSyncProtocol &&_fRunProtocol
			, CStr const &_FileName
			, CStr const &_OldFileName
			, CStr const &_TempFileName
			, CStr const &_RelativeFileName
			, uint64 _FileLength
			, EDirectoryManifestSyncFlag _SyncFlags
			, CStr &o_RSyncID
			, TCFunctionMutable<TCContinuation<void> (TCAsyncResult<void> const &_Result)> &&_fOnDone
		 	, NCryptography::CHashDigest_SHA256 const &_ExpectedDigest
		)
	{
		CStr RSyncID = fg_RandomID();
		o_RSyncID = RSyncID;
		auto &RSyncContext = m_RSyncContexts[RSyncID];

		RSyncContext.m_ExpectedDigest = _ExpectedDigest;

		auto ActorSubscription = g_ActorSubscription / [=]() -> TCContinuation<void>
			{
				TCContinuation<void> DestroyContinuation;

				TCVector<CStr> TempFiles;

				bool bFailedHash = false;
				bool bGeneralFailure = false;

				auto pRsyncContext = m_RSyncContexts.f_FindEqual(RSyncID);
				if (pRsyncContext)
				{
					bFailedHash = pRsyncContext->m_bFailedHash;

					TempFiles = fg_Move(pRsyncContext->m_TempFileNames);
					DestroyContinuation = pRsyncContext->m_fRunProtocol.f_Destroy();
				}
				else
				{
					bGeneralFailure = true;
					DestroyContinuation.f_SetResult();
				}

				m_RSyncContexts.f_Remove(RSyncID);

				for (auto &TempFileName : TempFiles)
				{
					try
					{
						if (CFile::fs_FileExists(TempFileName))
							CFile::fs_DeleteFile(TempFileName);
					}
					catch (CExceptionFile const &_Exception)
					{
						DMibLogWithCategory(Mib/Cloud/BackupManager, Error, "Failed to cleanup tempfile for rsync: {}", _Exception);
					}
				}

				TCContinuation<void> Continuation;
				DestroyContinuation > [=](TCAsyncResult<void> &&_Result) mutable
					{
						if (bGeneralFailure)
						{
							TCAsyncResult<void> Result;
							Result.f_SetException(DMibErrorInstanceBackupManagerHashMismatch("General failure for RSync"));
 							_fOnDone(Result) > fg_DiscardResult();
							Continuation.f_SetException(fg_Move(Result));
						}
						else if (bFailedHash)
						{
							TCAsyncResult<void> Result;
							Result.f_SetException(DMibErrorInstanceBackupManagerHashMismatch("Digest does not match after RSync"));
							_fOnDone(Result) > fg_DiscardResult();
							Continuation.f_SetException(fg_Move(Result));
						}
						else if (!_Result)
						{
							_fOnDone(_Result) > fg_DiscardResult();
							Continuation.f_SetException(fg_Move(_Result));
						}
						else
						{
							_fOnDone(_Result) > Continuation / [Continuation, _Result]()
								{
									Continuation.f_SetResult();
								}
							;
						}
					}
				;
				return Continuation;
			}
		;

		f_SequenceSyncs
			(
			 	_RelativeFileName
				, [=, fRunProtocol = fg_Move(_fRunProtocol)](COnScopeExitShared &&_pCleanup) mutable
				{
					auto pRsyncContext = m_RSyncContexts.f_FindEqual(RSyncID);
					if (!pRsyncContext)
						return;

					auto &RSyncContext = *pRsyncContext;

					RSyncContext.m_SequenceSyncsCleanup = fg_Move(_pCleanup);

					if (CFile::fs_FileExists(_FileName, EFileAttrib_Directory))
						CFile::fs_DeleteDirectoryRecursive(_FileName);
					else if (CFile::fs_FileExists(_FileName, EFileAttrib_Link))
						CFile::fs_DeleteFile(_FileName);

					ERSyncClientFlag RSyncFlags = ERSyncClientFlag_TruncateOutput;

					RSyncContext.m_TempFileNames.f_Insert(_TempFileName);

					CFile::fs_CreateDirectory(CFile::fs_GetPath(_FileName));

					RSyncContext.m_RelativeFileName = _RelativeFileName;
					RSyncContext.m_AbsoluteFileName = _FileName;
					RSyncContext.m_FileLength = _FileLength;
					bool bUseOld = false;
					bool bNewExists = CFile::fs_FileExists(_FileName);
					bool bOldExists = !_OldFileName.f_IsEmpty() && CFile::fs_FileExists(_OldFileName);
					if (bNewExists || !bOldExists)
					{
						RSyncContext.m_File.f_Open(_FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
						RSyncContext.m_TempFile.f_Open(_TempFileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
						RSyncContext.m_pClient = fg_Construct(RSyncContext.m_File, RSyncContext.m_File, 256, 4*1024*1024, 8*1024*1024, &RSyncContext.m_TempFile, RSyncFlags);
					}
					else
					{
						bUseOld = true;
						RSyncContext.m_File.f_Open(_FileName, EFileOpen_Read | EFileOpen_Write | EFileOpen_DontTruncate | EFileOpen_ShareAll);
						RSyncContext.m_SourceFile.f_Open(_OldFileName, EFileOpen_Read | EFileOpen_ShareAll);
						RSyncContext.m_pClient = fg_Construct(RSyncContext.m_SourceFile, RSyncContext.m_File, 256, 4*1024*1024, 8*1024*1024, nullptr, RSyncFlags);
					}

					DMibLogWithCategory
						(
							Mib/Cloud/BackupManager
							, Debug
							, "Start RSync protocol for file '{}' {}"
							"\n    FileName: {}"
							"\n    OldFileName: {}"
							"\n    Size: {}   UseOld: {}   bNewExists: {}   bOldExists: {}"
							, _RelativeFileName
							, (_SyncFlags & EDirectoryManifestSyncFlag_Append) != 0 ? "Append" : ""
							, _FileName
							, _OldFileName
							, _FileLength
							, bUseOld
							, bNewExists
							, bOldExists
						)
					;

					RSyncContext.m_fRunProtocol = fg_Move(fRunProtocol);

					f_RunRSyncProtocol(RSyncContext, {});
				}
			)
		;

		return fg_Explicit(fg_Move(ActorSubscription));
	}

	TCContinuation<TCActorSubscriptionWithID<>> CBackupInstance::f_StartRSync
		(
			CStr const &_FileName
		 	, CManifestFile const &_ManifestFile
			, FRunRSyncProtocol &&_fRunProtocol
		)
	{
		CStr ManifestError;
		if (!CBackupManagerBackup::fs_ManifestFileValid(_FileName, _ManifestFile, ManifestError))
			return DMibErrorInstance("Manifest change for '{}' is invalid: {}"_f << _FileName << ManifestError);

		auto &Internal = *mp_pInternal;

		if (auto pException = Internal.f_CheckFileName(_FileName, nullptr))
			return fg_Move(pException);

		CStr FileName = Internal.f_GetCurrentPath(_FileName);
		CStr OldFileName = Internal.f_GetLatestPath(_FileName);
		CStr TempFileName = fg_Format("{}.{}.tmp", FileName, fg_RandomID());
		CStr RSyncID;

		return Internal.f_StartRSyncShared
			(
				fg_Move(_fRunProtocol)
				, FileName
				, OldFileName
				, TempFileName
				, _FileName
				, _ManifestFile.m_Length
				, _ManifestFile.m_Flags
				, RSyncID
				, [this, _FileName, _ManifestFile](TCAsyncResult<void> const &_Result) -> TCContinuation<void>
				{
					auto &Internal = *mp_pInternal;
					if (_Result)
						return Internal.f_CommitFile(_FileName, _ManifestFile);
					return _Result;
				}
			 	, _ManifestFile.m_Digest
			)
		;
	}
}
