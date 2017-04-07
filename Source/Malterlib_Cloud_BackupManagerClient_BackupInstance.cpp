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
			, CBackupManagerBackup::CManifest const &_Manifest
			, CBackupManagerClient::CConfig const &_Config
			, CTrustedActorInfo const &_ActorInfo
			, TCWeakActor<CBackupManagerClient> const &_BackupManagerClient
			, CBackupManager::CBackupKey const &_BackupKey
		)
		: mp_BackupManager(_BackupManager)
		, mp_Manifest(_Manifest)
		, mp_Config(_Config)
		, mp_ActorInfo(_ActorInfo)
		, mp_BackupManagerClient(_BackupManagerClient)
		, mp_BackupKey(_BackupKey)
	{
		fp_StartBackup();
	}
	
	CBackupManagerClient_Instance::~CBackupManagerClient_Instance()
	{
	}
	
	CBackupManagerClient_Instance::CPendingBackupFile::~CPendingBackupFile()
	{
		auto pRunningState = m_pRunningState.f_Lock();
		if (pRunningState)
			pRunningState->m_pOnScopeExit.f_Clear();
	}
	
	TCContinuation<void> CBackupManagerClient_Instance::fp_Destroy()
	{
		auto pCanDestroyTracker = fg_Move(mp_pCanDestroyTracker);
		DMibCheck(pCanDestroyTracker);
		
		mp_PendingFiles.f_Clear();
		
		mp_FileManifestSequencer.f_Abort() > pCanDestroyTracker->f_Track();
		
		return pCanDestroyTracker->m_Continuation;
	}

	void CBackupManagerClient_Instance::fp_BackupNotification(CBackupManagerClient::CNotification &&_Notification)
	{
		mp_BackupManagerClient(&CBackupManagerClient::fp_OnNotification, mp_ActorInfo.m_HostInfo, fg_Move(_Notification)) > fg_DiscardResult();
	}
	
	void CBackupManagerClient_Instance::fp_ReportBackupFailed(CStr const &_Error)
	{
		DMibLogCategoryStr(mp_Config.m_LogCategory);
		DMibLog(Error, "{}", _Error);
		fp_BackupNotification(CBackupManagerClient::CNotification_BackupFailed{_Error});
	}
	
	CBackupManagerClient_Instance::CRunningSyncState::CRunningSyncState() = default;
	CBackupManagerClient_Instance::CRunningSyncState::~CRunningSyncState() = default;
	
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
					
					if (PendingFile.m_bFinished)
					{
						fp_FileFinished(FileName);
						fp_BackupNotification(CBackupManagerClient::CNotification_FileFinished{FileName});
					}
					
					if (PendingFile.m_bReschedule)
					{
						PendingFile.m_bReschedule = false;
						PendingFile.m_StartedContinuations.f_Clear();
						mp_PendingFilesQueue.f_Insert(PendingFile);
					}
					else
						mp_PendingFiles.f_Remove(pPendingFile);
					
					fp_ProcessBackupQueue();
				}
			;
			
			CStr FullPath = CFile::fs_AppendPath(mp_Config.m_Root, FileName);
			
			try
			{
				RunningState.m_File.f_Open(FullPath, EFileOpen_Read | EFileOpen_ShareRead | EFileOpen_ShareWrite);
				RunningState.m_pRSyncServer = fg_Construct(RunningState.m_File, 8*1024*1024);
			}
			catch (CExceptionFile const &_Exception)
			{
				fp_ReportBackupFailed(fg_Format("Failed to prepare rsync: {}", _Exception));
				return;
			}
			
			PendingFile.m_pRunningState = pRunningState;
			
			DMibCallActor
				(
					mp_Backup
					, CBackupManagerBackup::f_StartRSync 
					, FileName
					, g_ActorFunctor
					(
						g_ActorSubscription > [this, pRunningState]
						{
							pRunningState->m_pOnScopeExit.f_Clear();
						}
					) 
					> [this, pRunningState, FileName](CSecureByteVector &&_Packet) mutable -> TCContinuation<CSecureByteVector>
					{
						return TCContinuation<CSecureByteVector>::fs_RunProtected() > [&]() -> CSecureByteVector
							{
								NContainer::CSecureByteVector ToSendToClient;
								if (pRunningState->m_pRSyncServer->f_ProcessPacket(_Packet, ToSendToClient))
								{
									auto *pPendingFile = mp_PendingFiles.f_FindEqual(FileName);
									if (pPendingFile)
										pPendingFile->m_bFinished = true;
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
					
					for (auto &Continuatino : pPendingFile->m_StartedContinuations)
						Continuatino.f_SetResult();
					pPendingFile->m_StartedContinuations.f_Clear();
					
					if (!_Subscription)
					{
						fp_ReportBackupFailed(fg_Format("Failed to start rsync: {}", _Subscription.f_GetExceptionStr()));
						pRunningState->m_pOnScopeExit.f_Clear();
						return;
					}
					
					pPendingFile->m_RSyncSubscription = fg_Move(*_Subscription);
				}
			;
		}
	}

	void CBackupManagerClient_Instance::fp_NewPendingFile(CStr const &_FileName)
	{
		if (mp_bInitialBackupFinished)
			return;
		if (mp_InitialBackupPendingAdded(_FileName).f_WasCreated())
			mp_InitialBackupPending[_FileName];
	}
	
	void CBackupManagerClient_Instance::fp_FileFinished(CStr const &_FileName)
	{
		if (!mp_InitialBackupPending.f_Remove(_FileName))
			return;
		if (!mp_InitialBackupPending.f_IsEmpty())
			return;
		if (mp_bInitialBackupFinished)
			return;

		mp_bInitialBackupFinished = true;

		DMibCallActor
			(
				mp_Backup
				, CBackupManagerBackup::f_InitialBackupFinished
			)
			> [this](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					return fp_ReportBackupFailed(fg_Format("Initial backup finished failed: {}", _Result.f_GetExceptionStr()));
			}
		;
	}
	
	void CBackupManagerClient_Instance::fp_StartBackup()
	{
		DMibCallActor
			(
				mp_BackupManager
				, CBackupManager::f_InitBackup
				, mp_BackupKey
				, g_ActorSubscription > [this]() -> TCContinuation<void>
				{
					if (mp_bDestroyed || !mp_Backup)
						return fg_Explicit();
		
					return mp_BackupManagerClient(&CBackupManagerClient::fp_OnNotification, mp_ActorInfo.m_HostInfo, CBackupManagerClient::CNotification_BackupAborted());
				}
			)
			> [this](TCAsyncResult<TCDistributedActorInterfaceWithID<CBackupManagerBackup>> &&_ActorInterface)
			{
				if (!_ActorInterface)
					return fp_ReportBackupFailed(fg_Format("Failed to init backup: {}", _ActorInterface.f_GetExceptionStr()));
				
				mp_Backup = fg_Move(*_ActorInterface);
				
				DMibCallActor(mp_Backup, CBackupManagerBackup::f_StartBackup, mp_Manifest) > [this](TCAsyncResult<CBackupManagerBackup::CStartBackupResult> &&_StartResult)
					{
						if (!_StartResult)
						{
							mp_bBackupStartFailed = true;
							return fp_ReportBackupFailed(fg_Format("Failed to start backup: {}", _StartResult.f_GetExceptionStr()));
						}
						for (auto &FileToBackup : _StartResult->m_FilesNotUpToDate)
						{
							auto &FileName = _StartResult->m_FilesNotUpToDate.fs_GetKey(FileToBackup);
							
							auto *pManifestFile = mp_Manifest.m_Files.f_FindEqual(FileName);
							if (!pManifestFile)
							{
								DMibLogCategoryStr(mp_Config.m_LogCategory);
								DMibLog(Error, "Unexpected file in reply from backup server: {}", FileName);
								continue;
							}
							
							if (pManifestFile->f_IsDirectory())
								continue;
							
							auto &PendingFile = mp_PendingFiles[FileName];
							mp_PendingFilesQueue.f_Insert(PendingFile);
							fp_NewPendingFile(FileName);
						}
						
						for (auto &Changes : mp_PendingManifestChanges)
						{
							for (auto &Change : Changes)
								fp_ProcessManifestChange(mp_PendingManifestChanges.fs_GetKey(Changes), Change);
						}
						mp_PendingManifestChanges.f_Clear();
						
						mp_bBackupStarted = true;
						fp_ProcessBackupQueue();
					}
				;
			}
		;
	}

	TCContinuation<void> CBackupManagerClient_Instance::fp_AbortPendingFile(CStr const &_FileName)
	{
		TCContinuation<void> Continuation;
		auto *pPendingFile = mp_PendingFiles.f_FindEqual(_FileName);
		
		if (pPendingFile)
		{
			if (pPendingFile->m_Link.f_IsInList())
			{
				pPendingFile->m_Link.f_Unlink();
				Continuation.f_SetResult();
			}
			else
			{
				if (pPendingFile->m_RSyncSubscription.f_IsEmpty())
				{
					pPendingFile->m_StartedContinuations.f_Insert() > [Continuation, _FileName, this](TCAsyncResult<void> &&)
						{
							auto *pPendingFile = mp_PendingFiles.f_FindEqual(_FileName);
							if (!pPendingFile || pPendingFile->m_RSyncSubscription.f_IsEmpty())
							{
								Continuation.f_SetResult();
								return;
							}
							
							pPendingFile->m_RSyncSubscription->f_Destroy() > Continuation;
						}
					;
				}
				else
					pPendingFile->m_RSyncSubscription->f_Destroy() > Continuation;
			}
		}
		else
			Continuation.f_SetResult();
		
		return Continuation;
	}

	void CBackupManagerClient_Instance::fp_ProcessManifestChange(CStr const &_FileName, CBackupManagerBackup::CManifestChange const &_ManifestChange)
	{
		mp_FileManifestSequencer > [=]() -> TCContinuation<void>
			{
				TCContinuation<void> PendingAbortContinuation;
				
				CStr AbortFileName = _FileName;
				
				if (_ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Remove)
					PendingAbortContinuation = fp_AbortPendingFile(_FileName);
				else if (_ManifestChange.f_GetTypeID() == CBackupManagerBackup::EManifestChange_Rename)
					PendingAbortContinuation = fp_AbortPendingFile(_ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>().m_FromFileName);
				else
					PendingAbortContinuation.f_SetResult();

				TCContinuation<void> Continuation;
				PendingAbortContinuation > [this, Continuation, _ManifestChange, _FileName](TCAsyncResult<void> &&_Result)
					{
						if (!_Result)
						{
							DMibLogCategoryStr(mp_Config.m_LogCategory);
 							DMibLog(Error, "Failed to abort pending file sync: {}", _Result.f_GetExceptionStr());
						}
						DMibCallActor(mp_Backup, CBackupManagerBackup::f_ManifestChange, _FileName, _ManifestChange)
							> [this, _FileName, _ManifestChange](TCAsyncResult<void> &&_Result)
							{
								if (!_Result)
								{
									fp_ReportBackupFailed(fg_Format("Failed to change manifest on remote: {}", _Result.f_GetExceptionStr()));
									return;
								}
								
								switch (_ManifestChange.f_GetTypeID())
								{
								case CBackupManagerBackup::EManifestChange_Remove:
									{
										mp_PendingFiles.f_Remove(_FileName);
										fp_FileFinished(_FileName);
										return;
									}
								case CBackupManagerBackup::EManifestChange_Rename:
									{
										auto &Change = _ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>();
										mp_PendingFiles.f_Remove(Change.m_FromFileName);
										fp_FileFinished(Change.m_FromFileName);
										break;
									}
								}
								
								auto *pManifestFile = mp_Manifest.m_Files.f_FindEqual(_FileName);
								if (!pManifestFile)
									return;
								
								if (pManifestFile->f_IsDirectory())
									return;
								
								auto Mapping = mp_PendingFiles(_FileName);
								auto &PendingFile = *Mapping;
								if (Mapping.f_WasCreated())
								{
									mp_PendingFilesQueue.f_Insert(PendingFile);
									fp_NewPendingFile(_FileName);
									fp_ProcessBackupQueue();
								}
								else if (!PendingFile.m_Link.f_IsInList())
									PendingFile.m_bReschedule = true;
							}
						;
					}
				;
				
				return Continuation;
			}
			> fg_DiscardResult()
		;
	}
	
	void CBackupManagerClient_Instance::f_ManifestChanged(CStr const &_FileName, CBackupManagerBackup::CManifestChange const &_ManifestChange)
	{
		switch (_ManifestChange.f_GetTypeID())
		{
		case CBackupManagerBackup::EManifestChange_Add:
			{
				auto &Change = _ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Add>();
				mp_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				break;
			}
		case CBackupManagerBackup::EManifestChange_Change:
			{
				auto &Change = _ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Change>();
				mp_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				break;
			}
		case CBackupManagerBackup::EManifestChange_Remove:
			{
				mp_Manifest.m_Files.f_Remove(_FileName);
				break;
			}
		case CBackupManagerBackup::EManifestChange_Rename:
			{
				auto &Change = _ManifestChange.f_Get<CBackupManagerBackup::EManifestChange_Rename>();
				
				mp_Manifest.m_Files.f_Remove(Change.m_FromFileName);
				mp_Manifest.m_Files[_FileName] = Change.m_ManifestFile;
				break;
			}
		}
		
		if (mp_Backup)
		{
			if (mp_bBackupStarted)
				fp_ProcessManifestChange(_FileName, _ManifestChange);
			else if (!mp_bBackupStartFailed)
				mp_PendingManifestChanges[_FileName].f_Insert(_ManifestChange);
		}
	}
}
