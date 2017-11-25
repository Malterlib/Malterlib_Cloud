
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/Actor/Timer>

namespace NMib::NCloud::NBackupManager
{
	CBackupManagerServer::CBackupDownload::~CBackupDownload()
	{
		if (m_DirectorySyncSend)
		{
			m_DirectorySyncSend->f_DestroyNoResult(DMibPFile, DMibPLine);
			m_DirectorySyncSend.f_Clear();
		}
	}
	
	TCContinuation<void> CBackupManagerServer::CBackupDownload::f_Destroy()
	{
		auto DirectorySend = fg_Move(m_DirectorySyncSend);
		TCContinuation<void> Continuation;
		if (m_Subscription)
		{
			m_Subscription->f_Destroy().f_Dispatch().f_Timeout(10.0, "Timed out waiting for backup download to destroy")
				> [Continuation, DirectorySend = fg_Move(DirectorySend)](auto &&)
				{
					if (DirectorySend)
						DirectorySend->f_Destroy() > Continuation;
					else
						Continuation.f_SetResult();
				}
			;
		}
		else if (DirectorySend)
			DirectorySend->f_Destroy() > Continuation;
		else
			Continuation.f_SetResult();
		
		return Continuation;
	}

	CBackupManagerServer::CBackupDownload::CBackupDownload()
	{
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_DownloadBackup(CDownloadBackup &&_DownloadBackup)
		-> TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
	{
		auto pThis = m_pThis;
		
		if (pThis->f_IsDestroyed())
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		CStr FriendlyName;
		CStr HostID;
		
		if (!CBackupManager::fs_IsValidBackupSource(_DownloadBackup.m_BackupSource, &FriendlyName, &HostID))
			return Auditor.f_Exception({"Invalid backup source format", "(Download backup)"});

		if (_DownloadBackup.m_Time.f_IsValid())
			return Auditor.f_Exception({"Currently only latest backup download is supported", "(Download backup)"});

		auto &CallingHostID = fg_GetCallingHostID();
		
		TCVector<CStr> Permissions = {"Backup/ReadAll", fg_Format("Backup/Read/{}", _DownloadBackup.m_BackupSource)};
		if (HostID == CallingHostID)
			Permissions.f_Push("Backup/ReadSelf");

		TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>> Continuation;

		pThis->mp_Permissions.f_HasPermission("Start download backup", Permissions)
			> Continuation /
			[
			   	=
			   	, Subscription = fg_Move(_DownloadBackup.m_Subscription)
			 	, BackupSource = _DownloadBackup.m_BackupSource
			 	, Time = _DownloadBackup.m_Time
			 	, Desc = _DownloadBackup.f_GetDesc()
			]
			(bool _bHasPermission) mutable
			{
				if (!_bHasPermission)
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Download backup)"));

				auto pBackupSource = pThis->fp_GetBackupSource(BackupSource);

				if (!pBackupSource)
					return Continuation.f_SetException(Auditor.f_Exception({"No such backup source", "(Download backup)"}));

				(*pBackupSource)(&CBackupSource::f_CheckOutDirectory, Time)
					> Continuation % Auditor("Internal error checking out backup for download, check BackupManager log for details")
					/ [=, Subscription = fg_Move(Subscription)](CBackupSource::CCheckedOutDirectory &&_CheckedOutDirectory) mutable
					{
						CDirectorySyncSend::CConfig Config;
						Config.m_BasePath = _CheckedOutDirectory.m_Directory + "/Current";
						Config.m_Manifest = _CheckedOutDirectory.m_Directory + "/Manifest.bin";

						NStr::CStr DownloadID = fg_RandomID();

						auto &Download = pThis->mp_BackupDownloads[DownloadID];

						Download.m_DirectorySyncSend = pThis->mp_AppState.m_DistributionManager->f_ConstructActor<CDirectorySyncSend>(fg_Move(Config));
						Download.m_Subscription = fg_Move(Subscription);

						TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncInterface
							{
								Download.m_DirectorySyncSend->f_ShareInterface<CDirectorySyncClient>()
								, g_ActorSubscription > [pThis, Auditor, DownloadID, Desc, CheckedOutDirectory = fg_Move(_CheckedOutDirectory)]() mutable -> TCContinuation<void>
								{
									auto *pDownload = pThis->mp_BackupDownloads.f_FindEqual(DownloadID);
									if (!pDownload)
										return fg_Explicit();

									TCContinuation<void> Continuation;
									pDownload->m_DirectorySyncSend(&CDirectorySyncSend::f_GetResult)
										> [Auditor, Desc, Continuation, Subscription = fg_Move(pDownload->m_Subscription), CheckedOutDirectory = fg_Move(CheckedOutDirectory)]
										(TCAsyncResult<CDirectorySyncSend::CSyncResult> &&_Result) mutable
										{
											if (!_Result)
												Auditor.f_Error(fg_Format("'{}' Failed to get sync result for: {}", Desc, _Result.f_GetExceptionStr()));
											else
											{
												auto &Result = *_Result;

												CStr StatsMessage;
												uint64 nBytes = Result.m_Stats.m_IncomingBytes + Result.m_Stats.m_OutgoingBytes;

												StatsMessage = fg_Format
													(
														"{} files using {ns } bytes at {fe2} MB/s"
														, Result.m_Stats.m_nSyncedFiles
														, nBytes
														, (fp64(nBytes) / Result.m_Stats.m_nSeconds) / 1'000'000.0
													)
												;

												Auditor.f_Info(fg_Format("'{}' Backup download finished transferring: {}", Desc, StatsMessage));
											}

											CheckedOutDirectory.m_Subscription->f_Destroy() > Continuation / [Continuation, Subscription = fg_Move(Subscription)]()
												{
													Subscription->f_Destroy() > Continuation;
												}
											;
										}
									;

									pThis->mp_BackupDownloads.f_Remove(DownloadID);

									return Continuation;
								}
							}
						;

						Auditor.f_Info(fg_Format("'{}' Starting download of backup", Desc));

						Continuation.f_SetResult(fg_Move(SyncInterface));
					}
				;
			}
		;

		return Continuation;
	}
}
