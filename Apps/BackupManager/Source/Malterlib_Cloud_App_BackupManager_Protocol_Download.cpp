
#include "Malterlib_Cloud_App_BackupManager.h"
#include "Malterlib_Cloud_App_BackupManager_Internal.h"
#include <Mib/Cryptography/Hashes/SHA>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NBackupManager
{
	CBackupManagerServer::CBackupDownload::~CBackupDownload()
	{
		if (m_DirectorySyncSend)
			fg_Move(m_DirectorySyncSend).f_Destroy() > fg_DiscardResult();
	}
	
	TCFuture<void> CBackupManagerServer::CBackupDownload::f_Destroy()
	{
		auto DirectorySend = fg_Move(m_DirectorySyncSend);
		if (m_Subscription)
			co_await m_Subscription->f_Destroy().f_Timeout(10.0, "Timed out waiting for backup download to destroy").f_Wrap();

		if (DirectorySend)
			co_await fg_Move(DirectorySend).f_Destroy();

		co_return {};
	}

	CBackupManagerServer::CBackupDownload::CBackupDownload()
	{
	}

	auto CBackupManagerServer::CBackupManagerImplementation::f_DownloadBackup(CDownloadBackup &&_DownloadBackup)
		-> TCFuture<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
	{
		auto pThis = m_pThis;
		
		if (pThis->f_IsDestroyed())
			co_return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		CStr FriendlyName;
		CStr HostID;
		
		if (!CBackupManager::fs_IsValidBackupSource(_DownloadBackup.m_BackupSource, &FriendlyName, &HostID))
			co_return Auditor.f_Exception({"Invalid backup source format", "(Download backup)"});

		if (_DownloadBackup.m_Time.f_IsValid())
			co_return Auditor.f_Exception({"Currently only latest backup download is supported", "(Download backup)"});

		auto &CallingHostID = fg_GetCallingHostID();
		
		TCVector<CStr> Permissions = {"Backup/ReadAll", fg_Format("Backup/Read/{}", _DownloadBackup.m_BackupSource)};
		if (HostID == CallingHostID)
			Permissions.f_Push("Backup/ReadSelf");

		bool bHasPermission = co_await (pThis->mp_Permissions.f_HasPermission("Start download backup", Permissions) % "Permission denied downloading backup" % Auditor);
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Download backup)");

		auto pBackupSource = pThis->fp_GetBackupSource(_DownloadBackup.m_BackupSource);

		if (!pBackupSource)
			co_return Auditor.f_Exception({"No such backup source", "(Download backup)"});

		CBackupSource::CCheckedOutDirectory CheckedOutDirectory = co_await
			(
			 	(*pBackupSource)(&CBackupSource::f_CheckOutDirectory, _DownloadBackup.m_Time)
			 	% Auditor("Internal error checking out backup for download, check BackupManager log for details")
			)
		;
		CDirectorySyncSend::CConfig Config;
		Config.m_BasePath = CheckedOutDirectory.m_Directory + "/Current";
		Config.m_Manifest = CheckedOutDirectory.m_Directory + "/Manifest.bin";

		NStr::CStr DownloadID = fg_RandomID(pThis->mp_BackupDownloads);

		auto &Download = pThis->mp_BackupDownloads[DownloadID];

		Download.m_DirectorySyncSend = pThis->mp_AppState.m_DistributionManager->f_ConstructActor<CDirectorySyncSend>(fg_Move(Config));
		Download.m_Subscription = fg_Move(_DownloadBackup.m_Subscription);

		TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncInterface
			{
				Download.m_DirectorySyncSend->f_ShareInterface<CDirectorySyncClient>()
				, g_ActorSubscription / [pThis, Auditor, DownloadID, Desc = _DownloadBackup.f_GetDesc(), CheckedOutDirectory = fg_Move(CheckedOutDirectory)]() mutable -> TCFuture<void>
				{
					auto *pDownload = pThis->mp_BackupDownloads.f_FindEqual(DownloadID);
					if (!pDownload)
						co_return {};

					auto Subscription = fg_Move(pDownload->m_Subscription);

					auto GetResultFuture = g_Future <<= pDownload->m_DirectorySyncSend(&CDirectorySyncSend::f_GetResult);
					pThis->mp_BackupDownloads.f_Remove(DownloadID);

					auto GetResultResult = co_await fg_Move(GetResultFuture).f_Wrap();
					if (!GetResultResult)
						Auditor.f_Error(fg_Format("'{}' Failed to get sync result for: {}", Desc, GetResultResult.f_GetExceptionStr()));
					else
					{
						auto &Result = *GetResultResult;

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

					co_await CheckedOutDirectory.m_Subscription->f_Destroy();
					co_await Subscription->f_Destroy();

					co_return {};
				}
			}
		;

		Auditor.f_Info(fg_Format("'{}' Starting download of backup", _DownloadBackup.f_GetDesc()));

		co_return fg_Move(SyncInterface);
	}
}
