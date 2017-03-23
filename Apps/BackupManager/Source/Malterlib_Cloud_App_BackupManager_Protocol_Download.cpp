
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>

namespace NMib::NCloud::NBackupManager
{
	CBackupManagerServer::CBackupDownload::~CBackupDownload()
	{
		if (m_FileTransferSend)
		{
			m_FileTransferSend->f_DestroyNoResult(DMibPFile, DMibPLine);
			m_FileTransferSend.f_Clear();
		}
	}

	CBackupManagerServer::CBackupDownload::CBackupDownload()
	{
	}

	TCContinuation<CBackupManager::CStartDownloadBackup::CResult> CBackupManagerServer::CBackupManagerImplementation::f_StartDownloadBackup(CStartDownloadBackup &&_Params)
	{
		auto pThis = m_pThis;
		
		if (pThis->mp_bDestroyed)
			return DMibErrorInstance("Shutting down");

		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		CStr FriendlyName;
		CStr HostID;
		
		if (!CBackupManager::fs_IsValidBackupSource(_Params.m_BackupSource, &FriendlyName, &HostID))
			return Auditor.f_Exception({"Invalid backup source format", "(start download backup)"});

		if (!CBackupManager::fs_IsValidHostname(_Params.m_BackupID))
			return Auditor.f_Exception({"Invalid backup ID format", "(start download backup)"});
		
		auto &CallingHostID = fg_GetCallingHostID();
		
		bool bFullAccess = pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "Backup/ReadAll");
		
		while (!bFullAccess)
		{
			if (pThis->mp_Permissions.f_HostHasPermission(CallingHostID, "Backup/ReadSelf"))
			{
				if (HostID == CallingHostID)
					break;
			}
			if (pThis->mp_Permissions.f_HostHasPermission(CallingHostID, fg_Format("Backup/Read/{}", _Params.m_BackupSource)))
				break;
			
			return Auditor.f_AccessDenied("(Start download backup)");
		}
		
		TCContinuation<CBackupManager::CStartDownloadBackup::CResult> Continuation;
		
		NStr::CStr BackupSource = _Params.m_BackupSource;
		NTime::CTime Time = _Params.m_Time;
		NStr::CStr BackupID = _Params.m_BackupID;
		auto Desc = _Params.f_GetDesc();
		
		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
		auto fDoUpload = 
			[
				pThis
				, Desc
				, TransferContext = fg_Move(_Params.m_TransferContext)
				, Continuation
				, Auditor
			]
			(CStr const &_BackupPath) mutable
			{
				NStr::CStr DownloadID = fg_RandomID();
				
				auto &Download = pThis->mp_BackupDownloads[DownloadID];
				auto Cleanup = g_OnScopeExitActor > [pThis, DownloadID]
					{
						pThis->mp_BackupDownloads.f_Remove(DownloadID);
					}
				;
				
				Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(_BackupPath);
				Download.m_Desc = Desc;
				
				Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Move(TransferContext)) 
					> [pThis, Cleanup, Auditor, DownloadID, Continuation, Desc](TCAsyncResult<CActorSubscription> &&_Subscription)
					{
						if (!_Subscription)
						{
							CStr DetailedError = fg_Format("'{}' Failed to initialize backup download: {}", Desc, _Subscription.f_GetExceptionStr());
							Continuation.f_SetException(Auditor.f_Exception({"Failed to send files. See Backup Server log.", DetailedError}));
							return;
						}
						CBackupManager::CStartDownloadBackup::CResult Result;
						Result.m_Subscription = fg_Move(fg_Move(*_Subscription));
						Continuation.f_SetResult(fg_Move(Result));
						auto *pDownload = pThis->mp_BackupDownloads.f_FindEqual(DownloadID);
						if (!pDownload)
							return;
						auto &Download = *pDownload;
						Download.m_FileTransferSend(&CFileTransferSend::f_GetResult) > [pThis, Auditor, DownloadID, Cleanup, Desc](TCAsyncResult<CFileTransferResult> &&_Result)
							{
								if (!_Result)
									Auditor.f_Error(fg_Format("'{}' Failed to transfer backup download: {}", Desc, _Result.f_GetExceptionStr()));
								else
								{
									auto &Result = *_Result;
									CStr Message;
									if (!Result.m_nBytes)
										Message = "All files were already up to date";
									else
										Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);
									
									Auditor.f_Info(fg_Format("'{}' Backup download finished transferring: {}", Desc, Message));
								}
								
								auto *pDownload = pThis->mp_BackupDownloads.f_FindEqual(DownloadID);
								if (!pDownload)
									return;
								if (pDownload->m_FileTransferSend)
								{
									pDownload->m_FileTransferSend->f_DestroyNoResult(DMibPFile, DMibPLine);
									pDownload->m_FileTransferSend.f_Clear();
								}
								pThis->mp_BackupDownloads.f_Remove(DownloadID);
							}
						;
					}
				;
				
				Auditor.f_Info(fg_Format("'{}' Starting download of backup", Download.m_Desc));
			}
		;

		if (BackupID == "Latest")
		{
			fg_Dispatch
				(
					pThis->fp_GetQueryFileActor()
					, [ProgramDirectory, BackupSource]() -> CStr
					{
						CStr BaseBackups = fg_Format("{}/{}", ProgramDirectory, BackupSource);
						return CFile::fs_GetExpandedPath(CFile::fs_ResolveSymbolicLink(fg_Format("{}/Latest", BaseBackups)), BaseBackups);
					}
				)
				> [fDoUpload = fg_Move(fDoUpload), Continuation](TCAsyncResult<CStr> const &_Result) mutable
				{
					if (!_Result)
					{
						Continuation.f_SetException(DMibErrorInstance(fg_Format("Error getting latest backup path: {}", _Result.f_GetExceptionStr())));
						return;
					}
					fDoUpload(*_Result);
				}
			;
		}
		else
		{
			CStr BackupPath = fg_Format("{}/Backups/{}/{tst.,tsb_}_{}", ProgramDirectory, BackupSource, Time, BackupID);
			fDoUpload(BackupPath);
		}
		
		return Continuation;
	}
}
