// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_BackupManagerClient_BackupInstance.h"

namespace NMib::NCloud::NPrivate
{
	CBackupManagerClient_Instance::CBackupManagerClient_Instance
		(
			TCDistributedActor<CBackupManager> const &_BackupManager
			, CDirectoryManifest const &_Manifest
		 	, TCMap<CStr, CBackupManagerClient_ChecksumState> const &_InitialChecksumState
			, CBackupManagerClient::CConfig const &_Config
			, CTrustedActorInfo const &_ActorInfo
			, TCWeakActor<CBackupManagerClient> const &_BackupManagerClient
			, CBackupManager::CBackupKey const &_BackupKey
			, bool _bFinishedStarting
		)
		: mp_BackupManager(_BackupManager)
		, mp_Manifest(_Manifest)
		, mp_Config(_Config)
		, mp_ActorInfo(_ActorInfo)
		, mp_BackupManagerClient(_BackupManagerClient)
		, mp_BackupKey(_BackupKey)
		, mp_bFinishedStarting(_bFinishedStarting)
	{
		for (auto &ChecksumState : _InitialChecksumState)
		{
			auto &FileName = _InitialChecksumState.fs_GetKey(ChecksumState);

			auto &AppendState = mp_AppendFileState[FileName];
			AppendState.f_ChecksumState() = ChecksumState;
			AppendState.m_bDirty = false;
		}

		fp_StartBackup();
	}

	CBackupManagerClient_Instance::~CBackupManagerClient_Instance()
	{
	}

	CBackupManagerClient_Instance::CPendingBackupFile::~CPendingBackupFile()
	{
		auto pRunningState = m_pRunningState.f_Lock();
		if (pRunningState && pRunningState->m_pOnScopeExit)
		{
			DMibCloudBackupManagerDebugOut("Destructor clear on scope: {}\n", f_GetFileName());
			pRunningState->m_pOnScopeExit.f_Clear();
		}
	}

	TCFuture<void> CBackupManagerClient_Instance::fp_Destroy()
	{
		auto pCanDestroyTracker = fg_Move(mp_pCanDestroyTracker);
		DMibCheck(pCanDestroyTracker);

		mp_PendingFiles.f_Clear();
		mp_SequencedSyncs.f_Clear();

		if (mp_pManifestSyncState && mp_pManifestSyncState->m_RSyncSubscription)
			mp_pManifestSyncState->m_RSyncSubscription->f_Destroy() > pCanDestroyTracker->f_Track();

		return pCanDestroyTracker->m_Promise;
	}

	void CBackupManagerClient_Instance::fp_BackupNotification(CBackupManagerClient::CNotification &&_Notification)
	{
		mp_BackupManagerClient(&CBackupManagerClient::fp_OnNotification, mp_ActorInfo.m_HostInfo, fg_Move(_Notification)) > fg_DiscardResult();
	}

	void CBackupManagerClient_Instance::fp_ReportBackupError(CStr const &_Error, bool _bFatal)
	{
		DMibLogCategoryStr(mp_Config.m_LogCategory);
		DMibLog(Error, "{}", _Error);
		fp_BackupNotification(CBackupManagerClient::CNotification_BackupError{_Error, _bFatal});
	}

	CBackupManagerClient_Instance::CRunningSyncState::CRunningSyncState() = default;
	CBackupManagerClient_Instance::CRunningSyncState::~CRunningSyncState() = default;

	void CBackupManagerClient_Instance::fp_ReportHashMismatch(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, CPendingBackupFile &_PendingFile)
	{
		DMibCloudBackupManagerDebugOut("Hash mismatch: {}\n", _PendingFile.f_GetFileName());
		_PendingFile.m_bFinished = false;
		if (auto pAppendState = mp_AppendFileState.f_FindEqual(_pRunningState->m_FileName))
			pAppendState->m_bDirty = true;

		_pRunningState->m_pOnScopeExit.f_Clear();

		mp_BackupManagerClient(&CBackupManagerClient::fp_HashMismatch, _PendingFile.m_ManifestFile.m_OriginalPath) > fg_DiscardResult();
	}

	void CBackupManagerClient_Instance::fp_ReportRetry(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState)
	{
		DMibCloudBackupManagerDebugOut("Retry: {}\n", _pRunningState->m_FileName);
		if (auto pAppendState = mp_AppendFileState.f_FindEqual(_pRunningState->m_FileName))
			pAppendState->m_bDirty = true;

		_pRunningState->m_pOnScopeExit.f_Clear();

		mp_BackupManagerClient(&CBackupManagerClient::fp_HashMismatch, _pRunningState->m_ManifestFile.m_OriginalPath) > fg_DiscardResult();
	}

	void CBackupManagerClient_Instance::fp_RSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, CPendingBackupFile &_PendingFile, mint _SyncSequence)
	{
		auto &RunningState = *_pRunningState;

		CStr FullPath = CFile::fs_AppendPath(mp_Config.m_ManifestConfig.m_Root, _PendingFile.m_ManifestFile.m_OriginalPath);
		try
		{
			RunningState.m_File.f_Open(FullPath, EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_ShareBypass | EFileOpen_NoLocalCache);
			RunningState.m_LimitedFile.f_Open(&RunningState.m_File, 0, _PendingFile.m_ManifestFile.m_Length);
			RunningState.m_pRSyncServer = fg_Construct(RunningState.m_LimitedFile, 8*1024*1024);
		}
		catch (CExceptionFile const &_Exception)
		{
			fp_ReportRetry(_pRunningState);
			fp_ReportBackupError(fg_Format("Failed to prepare rsync: {}", _Exception), false);
			return;
		}

		auto FileName = RunningState.m_FileName;
		auto SyncSequence = _SyncSequence;
		auto pRunningState = _pRunningState;

		mp_Backup.f_CallActor(&CBackupManagerBackup::f_StartRSync)
			(
				RunningState.m_FileName
			 	, CBackupManagerBackup::CManifestFile{_PendingFile.m_ManifestFile}
				, g_ActorFunctor
				(
					g_ActorSubscription / [this, pRunningState, FileName, SyncFlags = _PendingFile.m_ManifestFile.m_Flags]() -> TCFuture<void>
					{
						auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);
						if (!pPendingFile)
						{
							DMibCloudBackupManagerDebugOut("Missing pending file: {}\n", FileName);
							pRunningState->m_pOnScopeExit.f_Clear();
							return fg_Explicit();
						}

						auto Subscription = fg_Move(pPendingFile->m_RSyncSubscription);

						TCFuture<void> DestroyFuture;
						if (!Subscription)
							DestroyFuture = fg_Explicit();
						else
							DestroyFuture = Subscription->f_Destroy();

						TCPromise<void> PromiseReturn;
						fg_Move(DestroyFuture) > [this, PromiseReturn, FileName, pRunningState, SyncFlags](TCAsyncResult<void> &&_Result)
							{
								if (auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName))
								{
									auto fReportHashMismatch = [&]
										{
											fp_ReportHashMismatch(pRunningState, *pPendingFile);
											PromiseReturn.f_SetResult();
										}
									;

									try
									{
										_Result.f_Access();
									}
									catch (CExceptionBackupManagerHashMismatch const &_Exception)
									{
										(void)_Exception;
										DMibLog(Info, "Reschedule: {}: {}", FileName, _Exception);
										return fReportHashMismatch();
									}
									catch (NException::CException const &)
									{
									}

									if (pPendingFile->m_bFinished && (SyncFlags & EDirectoryManifestSyncFlag_Append))
									{
										auto &RunningState = *pRunningState;
										auto &AppendState = mp_AppendFileState[RunningState.m_FileName];
										AppendState.m_Position = RunningState.m_ManifestFile.m_Length;
										AppendState.m_DigestState.f_Reset();
										RunningState.m_LimitedFile.f_SetPosition(0);
										uint8 Buffer[16*1024];
										for (auto Position = 0; Position < AppendState.m_Position;)
										{
											mint ThisTime = fg_Min(AppendState.m_Position - Position, 16*1024);
											RunningState.m_LimitedFile.f_ConsumeBytes(Buffer, ThisTime);
											AppendState.m_DigestState.f_AddData(Buffer, ThisTime);
											Position += ThisTime;
										}
										RunningState.m_LimitedFile.f_SetPosition(0);
										if (AppendState.m_DigestState.f_GetDigest() != RunningState.m_ManifestFile.m_Digest)
										{
											DMibLog(Info, "Reschedule (Append mismatch): {}", FileName);
											AppendState.m_bDirty = true;
											mp_BackupManagerClient(&CBackupManagerClient::fp_HashMismatch, pPendingFile->m_ManifestFile.m_OriginalPath) > fg_DiscardResult();
										}
										else
											AppendState.m_bDirty = false;
									}
								}

								DMibCloudBackupManagerDebugOut("RSync done clear on scope exit: {}\n", FileName);
								pRunningState->m_pOnScopeExit.f_Clear();
								PromiseReturn.f_SetResult(_Result);
							}
						;

						return PromiseReturn.f_MoveFuture();
					}
				)
				/ [this, pRunningState, FileName](CSecureByteVector &&_Packet) mutable -> TCFuture<CSecureByteVector>
				{
					return TCFuture<CSecureByteVector>::fs_RunProtected() / [&]() -> CSecureByteVector
						{
							NContainer::CSecureByteVector ToSendToClient;
							auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);

							if (pRunningState->m_pRSyncServer->f_ProcessPacket(_Packet, ToSendToClient))
							{
								if (pPendingFile)
									pPendingFile->m_bFinished = true;
							}

							if (pPendingFile)
							{
								pPendingFile->m_TransferStats.m_IncomingBytes += _Packet.f_GetLen();
								pPendingFile->m_TransferStats.m_OutgoingBytes += ToSendToClient.f_GetLen();
							}

							return ToSendToClient;
						}
					;
				}
			)
			> [this, FileName, SyncSequence, pRunningState](TCAsyncResult<TCActorSubscriptionWithID<>> &&_Subscription)
			{
				auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);
				if (!pPendingFile || pPendingFile->m_SyncSequence != SyncSequence)
					return;

				if (!_Subscription)
				{
					DMibCloudBackupManagerDebugOut("RSync start failed: {}\n", FileName);
					fp_ReportBackupError(fg_Format("Failed to start rsync: {}", _Subscription.f_GetExceptionStr()), false);
					pRunningState->m_pOnScopeExit.f_Clear();
					return;
				}

				pPendingFile->m_RSyncSubscription = fg_Move(*_Subscription);
			}
		;
	}

	auto CBackupManagerClient_Instance::fp_GetAppendFileCache(CStr const &_FileName) -> TCSharedPointer<CAppendFileCache>
	{
		CStr FullPath = CFile::fs_AppendPath(mp_Config.m_ManifestConfig.m_Root, _FileName);
		auto FileID = CFile::fs_GetUniqueIdentifier(FullPath);

		if (auto *pFile = mp_AppendFileCache.f_FindEqual(_FileName))
		{
			if ((*pFile)->m_FileID == FileID)
				return *pFile;

			(*pFile)->m_File.f_Close();
			(*pFile)->m_File.f_Open(FullPath, EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_ShareBypass | EFileOpen_NoLocalCache);
			(*pFile)->m_FileID = FileID;
			return (*pFile);
		}

		auto &pFile = mp_AppendFileCache[_FileName] = fg_Construct();
		auto &File = *pFile;
		File.m_File.f_Open(FullPath, EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_ShareBypass | EFileOpen_NoLocalCache);
		File.m_FileID = FileID;

		return pFile;
	}

	void CBackupManagerClient_Instance::fp_SendAppendSyncFile
		(
			TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState
			, TCPromise<bool> const &_Promise
			, uint64 _Length
			, TCSharedPointer<CAppendFileCache> const &_pFile
		 	, bool _bForceSync
		)
	{
		auto &RunningState = *_pRunningState;
		auto &AppendState = mp_AppendFileState[RunningState.m_FileName];
		auto &Position = AppendState.m_Position;

		bool bForceSync = _bForceSync;

		while ((bForceSync || Position < _Length) && RunningState.m_PendingQueue < mp_Config.m_MaxSendQueue)
		{
			bForceSync = false;
			mint ThisTime = fg_Min(_Length - Position, mp_Config.m_MaxSendQueue - RunningState.m_PendingQueue);

			CSecureByteVector Data;
			Data.f_SetLen(ThisTime);

			CBackupManagerBackup::CAppendData AppendData;
			AppendData.m_PreviousDigest = AppendState.m_DigestState.f_GetDigest();

			try
			{
				_pFile->m_File.f_SetPosition(Position);
				_pFile->m_File.f_Read(Data.f_GetArray(), Data.f_GetLen());
				AppendState.m_DigestState.f_AddData(Data.f_GetArray(), Data.f_GetLen());
			}
			catch (NException::CException const &_Exception)
			{
				if (!_Promise.f_IsSet())
					_Promise.f_SetException(DMibErrorInstance(fg_Format("Error reading append backup data: {}", _Exception)));
				return;
			}

			if (AppendState.m_DigestState.f_GetDigest() != RunningState.m_ManifestFile.m_Digest)
			{
				auto *pPendingFile = mp_PendingFiles.f_FindEqual(RunningState.m_FileName);
				if (pPendingFile)
				{
					DMibCloudBackupManagerDebugOut("Hash mismatch (Start append): {}\n", RunningState.m_FileName);
					fp_ReportHashMismatch(_pRunningState, *pPendingFile);
					_Promise.f_SetException(DMibErrorInstance(fg_Format("Hash mismatch reading apppend backup data, rescheduling")));
					return;
				}

				_Promise.f_SetException(DMibErrorInstance(fg_Format("Hash mismatch reading apppend backup data")));
				return;
			}

			AppendData.m_ManifestFile.f_ManifestFile() = RunningState.m_ManifestFile;
			AppendData.m_Position = Position;
			AppendData.m_Data = fg_Move(Data);

			mp_Backup.f_CallActor(&CBackupManagerBackup::f_AppendData)
				(
					RunningState.m_FileName
					, fg_Move(AppendData)
				)
				> [this, pRunningState = _pRunningState, ThisTime, _Promise, _Length, _pFile](TCAsyncResult<void> &&_Result)
				{
					auto &RunningState = *pRunningState;
					RunningState.m_PendingQueue -= ThisTime;

					auto *pPendingFile = mp_PendingFiles.f_FindEqual(RunningState.m_FileName);

					if (_Promise.f_IsSet())
						return;
					else if (!_Result)
					{
						if (pPendingFile)
						{
							try
							{
								_Result.f_Access();
							}
							catch (CExceptionBackupManagerHashMismatch const &)
							{
								DMibCloudBackupManagerDebugOut("Hash mismatch (End append): {}\n", RunningState.m_FileName);
								fp_ReportHashMismatch(pRunningState, *pPendingFile);
								_Promise.f_SetException(DMibErrorInstance(fg_Format("Hash mismatch appending backup data, rescheduling: {}", _Result.f_GetExceptionStr())));
								return;
							}
							catch (NException::CException const &)
							{
							}
						}

						_Promise.f_SetException(DMibErrorInstance(fg_Format("Error uploading append backup data: {}", _Result.f_GetExceptionStr())));
						return;
					}
					else if (pPendingFile)
						pPendingFile->m_TransferStats.m_OutgoingBytes += ThisTime;

					fp_SendAppendSyncFile(pRunningState, _Promise, _Length, _pFile, false);
				}
			;
			Position += ThisTime;
			RunningState.m_PendingQueue += ThisTime;
		}

		if (RunningState.m_PendingQueue == 0 && Position >= _Length)
		{
			if (!_Promise.f_IsSet())
				_Promise.f_SetResult(true);
		}
	}

	void CBackupManagerClient_Instance::fp_AppendSyncFile(TCSharedPointerSupportWeak<CRunningSyncState> const &_pRunningState, mint _SyncSequence)
	{
		auto &RunningState = *_pRunningState;

		auto FileName = RunningState.m_FileName;
		auto SyncSequence = _SyncSequence;
		auto pRunningState = _pRunningState;

		TCPromise<bool> Promise;

		try
		{
			auto pFileCache = fp_GetAppendFileCache(RunningState.m_ManifestFile.m_OriginalPath);

			fp_SendAppendSyncFile(_pRunningState, Promise, _pRunningState->m_ManifestFile.m_Length, pFileCache, true);
		}
		catch (NException::CException const &_Exception)
		{
			fp_ReportRetry(_pRunningState);
			fp_ReportBackupError(fg_Format("Failed to start append sync for '{}': {}", RunningState.m_FileName, _Exception), false);
			DMibCloudBackupManagerDebugOut("Append sync failed: {}\n", FileName);
			pRunningState->m_pOnScopeExit.f_Clear();
			return;
		}

		Promise.f_MoveFuture() > [=](TCAsyncResult<bool> &&_Result)
			{
				if (!_Result)
				{
					fp_ReportBackupError(fg_Format("Failed to append sync for '{}': {}", pRunningState->m_FileName, _Result.f_GetExceptionStr()), false);
					DMibCloudBackupManagerDebugOut("Append sync failed 2: {}\n", FileName);
					pRunningState->m_pOnScopeExit.f_Clear();
					return;
				}
				auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);
				if (*_Result && pPendingFile && pPendingFile->m_SyncSequence == SyncSequence)
				{
					pPendingFile->m_bFinished = true;
					pPendingFile->m_TransferStats.m_Type = CBackupManagerClient::EFileTransferType_Append;
					DMibCloudBackupManagerDebugOut("Append sync finished: {}\n", FileName);
				}
				else
				{
					DMibCloudBackupManagerDebugOut("Append sync not finished: {}\n", FileName);
				}
				pRunningState->m_pOnScopeExit.f_Clear();
			}
		;
	}

	void CBackupManagerClient_Instance::fp_ProcessBackupQueue()
	{
		while (true)
		{
			if (mp_nRunningSyncs >= mp_nMaxRunningSyncs || mp_bDestroyed)
				return;

			auto *pPendingFile = mp_PendingFilesQueue.f_Pop();
			if (!pPendingFile)
				return;

			auto &PendingFile = *pPendingFile;

			CStr FileName = PendingFile.f_GetFileName();

			mint SyncSequence = ++mp_SyncSequence;
			PendingFile.m_SyncSequence = SyncSequence;

			TCSharedPointerSupportWeak<CRunningSyncState> pRunningState = fg_Construct();
			auto &RunningState = *pRunningState;

			RunningState.m_pCanDestroyTracker = mp_pCanDestroyTracker;
			RunningState.m_FileName = FileName;
			RunningState.m_ManifestFile = PendingFile.m_ManifestFile;

			++mp_nRunningSyncs;
			RunningState.m_pOnScopeExit = g_OnScopeExitActor > [this, FileName, SyncSequence]
				{
					--mp_nRunningSyncs;

					auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);
					if (!pPendingFile || pPendingFile->m_SyncSequence != SyncSequence)
					{
						fp_ProcessBackupQueue();
						return;
					}

					auto &PendingFile = *pPendingFile;

					bool bFinished = PendingFile.m_bFinished;

					CBackupManagerClient::CFileTransferStats Stats = PendingFile.m_TransferStats;
					Stats.m_nSeconds = PendingFile.m_Clock.f_GetTime();
					if (Stats.m_nSeconds == 0.0)
						Stats.m_nSeconds = 0.000000001;

					auto ManifestFile = fg_Move(PendingFile.m_ManifestFile);
					mp_PendingFiles.f_Remove(pPendingFile);

					if (bFinished)
					{
						DMibCloudBackupManagerDebugOut("FINISHED: {}\n", FileName);
						mp_Manifest.m_Files[FileName] = fg_Move(ManifestFile);
						fp_BackupNotification(CBackupManagerClient::CNotification_FileFinished{FileName, Stats});
						fp_FileFinished(FileName);
					}
					else
						DMibCloudBackupManagerDebugOut("NOT FINISHED: {}\n", FileName);

					fp_ProcessBackupQueue();
				}
			;

			PendingFile.m_pRunningState = pRunningState;

			if (PendingFile.m_ManifestFile.m_Flags & EDirectoryManifestSyncFlag_Append)
			{
				auto &AppendState = mp_AppendFileState[FileName];
				if (AppendState.m_bDirty)
					fp_RSyncFile(pRunningState, PendingFile, SyncSequence);
				else
					fp_AppendSyncFile(pRunningState, SyncSequence);
			}
			else
				fp_RSyncFile(pRunningState, PendingFile, SyncSequence);
		}
	}

	void CBackupManagerClient_Instance::fp_NotQuiescent()
	{
		if (mp_bQuiescent)
		{
			mp_bQuiescent = false;
			fp_BackupNotification(CBackupManagerClient::CNotification_Unquiescent{});
		}
	}

	void CBackupManagerClient_Instance::fp_NewPendingFile(CStr const &_FileName)
	{
		fp_NotQuiescent();

		if (mp_bInitialBackupFinished)
			return;
		if (mp_InitialBackupPendingAdded(_FileName).f_WasCreated())
			mp_InitialBackupPending[_FileName];
	}

	void CBackupManagerClient_Instance::fp_CheckQuiescent()
	{
		if (mp_bClientActive)
			return;
		if (mp_nPendingManifestChanges)
			return;
		if (!mp_InitialBackupPending.f_IsEmpty())
			return;
		if (!mp_PendingFiles.f_IsEmpty())
			return;
		if (!mp_bFinishedStarting)
			return;
		if (!mp_bBackupStarted)
			return;
		if (!mp_bInitialBackupCommitted)
			return;

		if (mp_bQuiescent)
			return;
		mp_bQuiescent = true;

		fp_BackupNotification(CBackupManagerClient::CNotification_Quiescent{});
	}

	void CBackupManagerClient_Instance::fp_CheckInitialBackupFinished()
	{
		if (mp_bClientActive)
			return;
		if (mp_nPendingManifestChanges)
			return;
		if (!mp_InitialBackupPending.f_IsEmpty())
			return;
		if (!mp_bFinishedStarting)
			return;
		if (!mp_bBackupStarted)
			return;

		if (mp_bInitialBackupFinished)
			return;
		mp_bInitialBackupFinished = true;

		mp_Backup.f_CallActor(&CBackupManagerBackup::f_InitialBackupFinished)
			(
			 	mp_Config.m_bReportChangesInInitialFinished ? CBackupManagerBackup::EInitialBackupFinishedFlag_ReturnChanges : CBackupManagerBackup::EInitialBackupFinishedFlag_None
			)
			> [this](TCAsyncResult<CBackupManagerBackup::CInitialBackupFinishedResult> &&_Result)
			{
				DMibLogWithCategory(Mib/Cloud/BackupManagerClient, Debug, "Initial backup finished");

				if (!_Result)
					return fp_ReportBackupError(fg_Format("Initial backup finished failed: {}", _Result.f_GetExceptionStr()), true);

				auto &Result = *_Result;

				mp_bInitialBackupCommitted = true;
				fp_BackupNotification(CBackupManagerClient::CNotification_InitialFinished{Result.m_AddedFiles, Result.m_RemovedFiles, Result.m_UpdatedFiles});
				fp_CheckQuiescent();
			}
		;
	}

	void CBackupManagerClient_Instance::fp_FileFinished(CStr const &_FileName)
	{
		fp_CheckQuiescent();

		if (!mp_InitialBackupPending.f_Remove(_FileName))
			return;

		fp_CheckInitialBackupFinished();
	}

	TCFuture<void> CBackupManagerClient_Instance::fp_SyncManifest()
	{
		TCSharedPointer<CManifestSyncState> pState = fg_Construct();
		pState->m_ManifestStream << mp_Manifest;
		pState->m_ManifestStream.f_SetPosition(0);
		mp_pManifestSyncState = pState;
		auto ManifestDigest = CHash_SHA256::fs_DigestFromData(pState->m_ManifestStream.f_GetVector());

		try
		{
			pState->m_pRSyncServer = fg_Construct(pState->m_ManifestStream, 8*1024*1024);
		}
		catch (CExceptionFile const &_Exception)
		{
			return DMibErrorInstance(fg_Format("Failed to prepare manifest rsync: {}", _Exception));
		}

		TCPromise<void> Promise;

		mp_Backup.f_CallActor(&CBackupManagerBackup::f_StartManifestRSync)
			(
				g_ActorFunctor
				(
					g_ActorSubscription / [this, Promise]() -> TCFuture<void>
					{
						if (!mp_pManifestSyncState)
							return fg_Explicit();

						bool bDone = mp_pManifestSyncState->m_bDone;

						auto Subscription = fg_Move(mp_pManifestSyncState->m_RSyncSubscription);
						mp_pManifestSyncState.f_Clear();

						TCFuture<void> DestroyFuture;
						if (!Subscription)
							DestroyFuture = fg_Explicit();
						else
							DestroyFuture = Subscription->f_Destroy();

						TCPromise<void> PromiseReturn;
						fg_Move(DestroyFuture) > [Promise, PromiseReturn, bDone](TCAsyncResult<void> &&_Result)
							{
								if (!Promise.f_IsSet())
								{
									do
									{
										try
										{
											_Result.f_Access();
										}
										catch (CExceptionBackupManagerHashMismatch const &_Exception)
										{
											Promise.f_SetException(_Exception);
											break;
										}
										catch (NException::CException const &)
										{
										}

										if (bDone)
											Promise.f_SetResult();
										else
											Promise.f_SetException(DMibErrorInstance("Manifest rsync aborted"));
									}
									while (false)
										;
								}

								PromiseReturn.f_SetResult(_Result);
							}
						;

						return PromiseReturn.f_MoveFuture();
					}
				)
				/ [this, Promise](CSecureByteVector &&_Packet) mutable -> TCFuture<CSecureByteVector>
				{
					if (!mp_pManifestSyncState)
						return DMibErrorInstance("Aborted");

					return TCFuture<CSecureByteVector>::fs_RunProtected() / [&]() -> CSecureByteVector
						{
							NContainer::CSecureByteVector ToSendToClient;
							if (mp_pManifestSyncState->m_pRSyncServer->f_ProcessPacket(_Packet, ToSendToClient))
								mp_pManifestSyncState->m_bDone = true;

							return ToSendToClient;
						}
					;
				}
				, pState->m_ManifestStream.f_GetLength()
			 	, ManifestDigest
			)
			> [this, Promise](TCAsyncResult<TCActorSubscriptionWithID<>> &&_Subscription)
			{
				if (!_Subscription)
				{
					Promise.f_SetException(DMibErrorInstance(fg_Format("Manifest start rsync failed: ", _Subscription.f_GetExceptionStr())));
					return;
				}

				if (!mp_pManifestSyncState)
					return;

				mp_pManifestSyncState->m_RSyncSubscription = fg_Move(*_Subscription);
			}
		;

		return Promise.f_MoveFuture();
	}

	void CBackupManagerClient_Instance::fp_StartBackup()
	{
		CBackupManager::CInitBackup InitParams;

		InitParams.m_BackupKey = mp_BackupKey;
		InitParams.m_Subscription = g_ActorSubscription / [this]() -> TCFuture<void>
			{
				if (mp_bDestroyed || !mp_Backup)
					return fg_Explicit();

				return mp_BackupManagerClient(&CBackupManagerClient::fp_OnNotification, mp_ActorInfo.m_HostInfo, CBackupManagerClient::CNotification_BackupAborted());
			}
		;

		mp_BackupManager.f_CallActor(&CBackupManager::f_InitBackup)(fg_Move(InitParams))
			> [this](TCAsyncResult<TCDistributedActorInterfaceWithID<CBackupManagerBackup>> &&_ActorInterface)
			{
				if (!_ActorInterface)
					return fp_ReportBackupError(fg_Format("Failed to init backup: {}", _ActorInterface.f_GetExceptionStr()), true);

				mp_Backup = fg_Move(*_ActorInterface);

				fp_SyncManifest() > [this](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							mp_bBackupStartFailed = true;
							return fp_ReportBackupError(fg_Format("Failed to sync manifest for backup: {}", _Result.f_GetExceptionStr()), true);
						}

						mp_Backup.f_CallActor(&CBackupManagerBackup::f_StartBackup)() > [this](TCAsyncResult<CBackupManagerBackup::CStartBackupResult> &&_StartResult)
							{
								if (!_StartResult)
								{
									mp_bBackupStartFailed = true;
									return fp_ReportBackupError(fg_Format("Failed to start backup: {}", _StartResult.f_GetExceptionStr()), true);
								}
								for (auto &Digest : _StartResult->m_FilesNotUpToDate)
								{
									auto &FileName = _StartResult->m_FilesNotUpToDate.fs_GetKey(Digest);
									auto *pManifestFile = mp_Manifest.m_Files.f_FindEqual(FileName);
									if (!pManifestFile)
									{
										DMibLogCategoryStr(mp_Config.m_LogCategory);
										DMibLog(Error, "Unexpected file in reply from backup server: {}", FileName);
										continue;
									}

									if (!pManifestFile->f_IsFile())
									{
										DMibLogCategoryStr(mp_Config.m_LogCategory);
										DMibLog(Error, "Non-file sent as not up to date: {}", FileName);
										continue;
									}

									TCVector<CStr> FilesToSequenceWrite = {FileName};
									TCVector<CStr> FilesToSequenceRead = {};
									for (CStr ParentDir = CFile::fs_GetPath(FileName); !ParentDir.f_IsEmpty(); ParentDir = CFile::fs_GetPath(ParentDir))
										FilesToSequenceRead.f_Insert(ParentDir);

									fp_NewPendingFile(FileName);

									fp_SequenceMultipleSyncs
										(
											[=, ManifestFile = *pManifestFile](NMib::COnScopeExitShared &&_pScope) mutable
											{
												auto &PendingFile = mp_PendingFiles[FileName];
												mp_PendingFilesQueue.f_Insert(PendingFile);
												PendingFile.m_ManifestFile = ManifestFile;
												PendingFile.m_pSequenceSyncsScope = _pScope;
												fp_ProcessBackupQueue();
											}
										 	, FilesToSequenceWrite
										 	, FilesToSequenceRead
										)
									;

									pManifestFile->m_Digest = Digest;
								}

								for (auto &Change : mp_PendingManifestChanges)
									fp_ProcessManifestChange(Change.m_FileName, Change.m_Info);
								mp_PendingManifestChanges.f_Clear();

								mp_bBackupStarted = true;
								fp_CheckInitialBackupFinished();
								fp_CheckQuiescent();
							}
						;
					}
				;
			}
		;
	}

	void CBackupManagerClient_Instance::fp_ProcessManifestChange(CStr const &_FileName, CManifestChangeInfo const &_ManifestChange)
	{
		TCVector<CStr> FilesToSequenceWrite = {_FileName};
		TCVector<CStr> FilesToSequenceRead = {};

		auto fAddParents = [&](CStr const &_FileName)
			{
				for (CStr ParentDir = CFile::fs_GetPath(_FileName); !ParentDir.f_IsEmpty(); ParentDir = CFile::fs_GetPath(ParentDir))
					FilesToSequenceRead.f_Insert(ParentDir);
			}
		;

		fAddParents(_FileName);

		CStr SourceFile;

		if (_ManifestChange.m_ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Rename)
		{
			auto &FromFile = _ManifestChange.m_ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>().m_FromFileName;
			FilesToSequenceWrite.f_Insert(FromFile);
			fAddParents(FromFile);
			SourceFile = FromFile;
		}
		else
			SourceFile = _FileName;

		++mp_nPendingManifestChanges;
		auto Cleanup = g_OnScopeExitActor > [this]
			{
				--mp_nPendingManifestChanges;
				if (!mp_nPendingManifestChanges)
				{
					fp_CheckQuiescent();
					fp_CheckInitialBackupFinished();
				}
			}
		;
		fp_NotQuiescent();

#ifdef DMibCloudBackupManagerDebug
		switch (_ManifestChange.m_ManifestChange.f_GetTypeID())
		{
		case CBackupManagerBackup::EManifestChange_Change:
			DMibCloudBackupManagerDebugOut("^^^ Change {}   Write {vs}   Read {vs}\n", _FileName, FilesToSequenceWrite, FilesToSequenceRead);
			break;
		case CBackupManagerBackup::EManifestChange_Add:
			DMibCloudBackupManagerDebugOut("^^^ Add {}   Write {vs}   Read {vs}\n", _FileName, FilesToSequenceWrite, FilesToSequenceRead);
			break;
		case CBackupManagerBackup::EManifestChange_Remove:
			DMibCloudBackupManagerDebugOut("^^^ Remove {}   Write {vs}   Read {vs}\n", _FileName, FilesToSequenceWrite, FilesToSequenceRead);
			break;
		case CBackupManagerBackup::EManifestChange_Rename:
			DMibCloudBackupManagerDebugOut
				(
				 	"^^^ Rename {} -> {}   Write {vs}   Read {vs}\n"
				 	, _ManifestChange.m_ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>().m_FromFileName
				 	, _FileName
				 	, FilesToSequenceWrite
				 	, FilesToSequenceRead
				)
			;
			break;
		}
#endif

		fp_SequenceMultipleSyncs
			(
				[
				 	=
				 	, ChangePromise = _ManifestChange.m_Promise
				 	, ManifestChange = _ManifestChange.m_ManifestChange
				 	, bDirty = _ManifestChange.m_bDirty
				]
			 	(NMib::COnScopeExitShared &&_pScope) mutable
				{
#ifdef DMibCloudBackupManagerDebug
					switch (ManifestChange.f_GetTypeID())
					{
					case CBackupManagerBackup::EManifestChange_Change: DMibCloudBackupManagerDebugOut("<<< Change {}\n", _FileName); break;
					case CBackupManagerBackup::EManifestChange_Add: DMibCloudBackupManagerDebugOut("<<< Add {}\n", _FileName); break;
					case CBackupManagerBackup::EManifestChange_Remove: DMibCloudBackupManagerDebugOut("<<< Remove {}\n", _FileName); break;
					case CBackupManagerBackup::EManifestChange_Rename:
						DMibCloudBackupManagerDebugOut("<<< Rename {} -> {}\n", ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>().m_FromFileName, _FileName);
						break;
					}
#endif
					auto *pManifestFile = mp_Manifest.m_Files.f_FindEqual(SourceFile);
					bool bFile;

					NCryptography::CHashDigest_SHA256 NewRenameDigest;

					if (!pManifestFile)
					{
						if (ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Rename)
						{
							auto &RenameChange = ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>();
							bFile = RenameChange.m_ManifestFile.f_IsFile();

							CBackupManagerBackup::CManifestChange_Add NewChange;
							NewChange.m_ManifestFile = RenameChange.m_ManifestFile;
							ManifestChange = NewChange;
						}
						else if (ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Add)
						{
							auto &AddChange = ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Add>();
							bFile = AddChange.m_ManifestFile.f_IsFile();
						}
						else if (ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Change)
						{
							auto &AddChange = ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Change>();
							bFile = AddChange.m_ManifestFile.f_IsFile();

							CBackupManagerBackup::CManifestChange_Add NewChange;
							NewChange.m_ManifestFile = AddChange.m_ManifestFile;
							ManifestChange = NewChange;
						}
						else
						{
							DMibCloudBackupManagerDebugOut("IGNORE remove: {} ({})\n", SourceFile, _FileName);
							ChangePromise.f_SetResult();
							return;
						}
					}
					else
					{

						if (ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Remove)
							bFile = pManifestFile->f_IsFile();
						else
						{
							auto &ManifestFile = CBackupManagerBackup::fs_ManifestFileFromChange(ManifestChange);
							bFile = ManifestFile.f_IsFile();
						}
						if (ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Rename)
						{
							auto &RenameChange = ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>();
							NewRenameDigest = RenameChange.m_ManifestFile.m_Digest;
							RenameChange.m_ManifestFile.m_Digest = pManifestFile->m_Digest;
						}
					}

					if (!CBackupManagerBackup::fs_PretendApplyManifestChange(mp_Manifest, _FileName, ManifestChange))
					{
						DMibCloudBackupManagerDebugOut("NOT CHANGED: {} ({})\n", SourceFile, _FileName);
						ChangePromise.f_SetResult();
						return;
					}

					auto fSyncFile = [=](CBackupManagerBackup::CManifestChange const &_ManifestChange) -> bool
						{
#if defined DMibContractConfigure_CheckEnabled
							CStr ManifestError;
							DMibCheck(CBackupManagerBackup::fs_ManifestChangeValid(_FileName, _ManifestChange, ManifestError));
#endif
							(void)Cleanup;
							auto &ManifestFile = CBackupManagerBackup::fs_ManifestFileFromChange(_ManifestChange);
							if (!ManifestFile.f_IsFile())
							{
								ChangePromise.f_SetResult();
								return false;
							}

							if (bDirty && (ManifestFile.m_Flags & EDirectoryManifestSyncFlag_Append))
							{
								auto &AppendState = mp_AppendFileState[_FileName];
								AppendState.m_bDirty = true;
							}

							auto Mapping = mp_PendingFiles(_FileName);
							auto &PendingFile = *Mapping;
							PendingFile.m_pSequenceSyncsScope = _pScope;
							PendingFile.m_ManifestFile = ManifestFile;

							DMibCheck(Mapping.f_WasCreated());
							DMibCheck(!PendingFile.m_Link.f_IsInList());

							mp_PendingFilesQueue.f_Insert(PendingFile);
							fp_NewPendingFile(_FileName);
							fp_ProcessBackupQueue();

							ChangePromise.f_SetResult();

							return true;
						}
					;

					if
						(
							!bFile
							|| ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Remove
							|| ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Rename
						)
					{
						if (bFile)
							fp_NewPendingFile(_FileName);

						mp_Backup.f_CallActor(&CBackupManagerBackup::f_ManifestChange)(_FileName, ManifestChange) > [=, Clock = NTime::CClock{true}](TCAsyncResult<void> &&_Result) mutable
							{
								(void)Cleanup;
								(void)_pScope;

								if (!_Result)
								{
									fp_ReportBackupError(fg_Format("Failed to change manifest on remote: {}", _Result.f_GetExceptionStr()), false);
									ChangePromise.f_SetResult();
									return;
								}

								switch (ManifestChange.f_GetTypeID())
								{
								case CBackupManagerBackup::EManifestChange_Remove:
									{
										mp_PendingFiles.f_Remove(_FileName);
										if (bFile)
										{
											CBackupManagerClient::CFileTransferStats Stats;
											Stats.m_Type = CBackupManagerClient::EFileTransferType_Delete;
											Stats.m_nSeconds = Clock.f_GetTime();
											if (Stats.m_nSeconds == 0.0)
												Stats.m_nSeconds = 0.000000001;
											fp_BackupNotification(CBackupManagerClient::CNotification_FileFinished{_FileName, Stats});
											fp_FileFinished(_FileName);
										}
										CBackupManagerBackup::fs_ApplyManifestChange(mp_Manifest, _FileName, ManifestChange);
										ChangePromise.f_SetResult();
										return;
									}
								case CBackupManagerBackup::EManifestChange_Rename:
									{
										auto &Change = ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>();
										mp_PendingFiles.f_Remove(Change.m_FromFileName);
										if (bFile)
										{
											CBackupManagerClient::CFileTransferStats Stats;
											Stats.m_Type = CBackupManagerClient::EFileTransferType_Rename;
											Stats.m_nSeconds = Clock.f_GetTime();
											if (Stats.m_nSeconds == 0.0)
												Stats.m_nSeconds = 0.000000001;
											fp_BackupNotification(CBackupManagerClient::CNotification_FileFinished{_FileName, Stats});
											fp_FileFinished(Change.m_FromFileName);

											CBackupManagerBackup::fs_ApplyManifestChange(mp_Manifest, _FileName, ManifestChange);
											if (Change.m_ManifestFile.m_Digest == NewRenameDigest)
											{
												DMibCloudBackupManagerDebugOut("Rename MATCH digest: {} -> {}\n", SourceFile, _FileName);
												ChangePromise.f_SetResult();
												return;
											}
											else
											{
												DMibCloudBackupManagerDebugOut("Rename NO MATCH digest: {} -> {}\n", SourceFile, _FileName);
												Change.m_ManifestFile.m_Digest = NewRenameDigest;
											}
										}
										break;
									}
								}

								if (!fSyncFile(ManifestChange))
									CBackupManagerBackup::fs_ApplyManifestChange(mp_Manifest, _FileName, ManifestChange);
							}
						;

					}
					else
					{
						fSyncFile(ManifestChange);
					}
				}
			 	, FilesToSequenceWrite
			 	, FilesToSequenceRead
			)
		;
	}

	TCFuture<void> CBackupManagerClient_Instance::f_ManifestChanged
		(
		 	CStr const &_FileName
		 	, CBackupManagerBackup::CManifestChange const &_ManifestChange
		 	, bool _bDirty
		 	, CBackupManagerClient_ChecksumState const &_ChecksumState
		)
	{
#if defined DMibContractConfigure_CheckEnabled
		CStr ManifestError;
		DMibCheck(CBackupManagerBackup::fs_ManifestChangeValid(_FileName, _ManifestChange, ManifestError));
		if (_ManifestChange.f_GetTypeID() != CBackupManagerBackup::EManifestChange_Remove)
		{
			auto &File = CBackupManagerBackup::fs_ManifestFileFromChange(_ManifestChange);
			DMibCheck
				(
				 	!File.f_IsFile()
				 	|| !(File.m_Flags & EDirectoryManifestSyncFlag_Append)
				 	|| File.m_Digest == _ChecksumState.m_DigestState.f_GetDigest()
				)
			;
		}
#endif

		TCPromise<void> Promise;

		if (mp_Backup)
		{
			if (mp_bBackupStarted)
				fp_ProcessManifestChange(_FileName, {_ManifestChange, Promise, _bDirty});
			else if (!mp_bBackupStartFailed)
				mp_PendingManifestChanges.f_Insert({_FileName, {_ManifestChange, Promise, _bDirty}});
			else
				Promise.f_SetResult();
		}
		else
		{
			CBackupManagerBackup::fs_ApplyManifestChange(mp_Manifest, _FileName, _ManifestChange);
			Promise.f_SetResult();
		}

		return Promise.f_MoveFuture();
	}

	void CBackupManagerClient_Instance::f_MarkActive(bool _bActive)
	{
		DMibCloudBackupManagerDebugOut("Mark active: {}\n", _bActive);

		mp_bClientActive = _bActive;
		if (!_bActive)
		{
			fp_CheckInitialBackupFinished();
			fp_CheckQuiescent();
		}
	}

	void CBackupManagerClient_Instance::f_BackupFinishedStarting()
	{
		mp_bFinishedStarting = true;

		DMibLogWithCategory
			(
				Mib/Cloud/BackupClient
				, Debug
				, "Finished starting backup"
			)
		;

		if (mp_Backup)
		{
			fp_CheckInitialBackupFinished();
			fp_CheckQuiescent();
		}
	}
}
