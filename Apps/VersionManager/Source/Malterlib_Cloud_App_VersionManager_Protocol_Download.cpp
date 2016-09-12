// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CVersionDownload::~CVersionDownload()
	{
		if (m_FileTransferSend)
		{
			m_FileTransferSend->f_Destroy();
			m_FileTransferSend.f_Clear();
		}
	}

	CVersionManagerDaemonActor::CServer::CVersionDownload::CVersionDownload()
	{
	}

	auto CVersionManagerDaemonActor::CServer::fp_Protocol_DownloadVersion(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CStartDownloadVersion &&_Params) 
		-> TCContinuation<CVersionManager::CStartDownloadVersion::CResult> 
	{
		if (!mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
		{
			CStr Error = "Invalid application format";
			fsp_LogActivityError(_CallingHostInfo, Error + " (start download version)");
			return DMibErrorInstance(Error);
		}

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionID, ErrorStr))
			{
				CStr Error = fg_Format("Invalid version ID format: {}", ErrorStr);
				fsp_LogActivityError(_CallingHostInfo, Error + " (start download version)");
				return DMibErrorInstance(Error);
			}
		}
		
		bool bFullAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "Application/ReadAll");
		
		while (!bFullAccess)
		{
			if (mp_Permissions.f_HostHasPermission(_CallingHostInfo.f_GetRealHostID(), fg_Format("Version/Read/{}", _Params.m_Application)))
				break;
			
			return fp_AccessDenied(_CallingHostInfo, "Start download version");
		}
		
		NStr::CStr DownloadID = fg_RandomID();
		
		auto &Download = mp_VersionDownloads[DownloadID];
		auto Cleanup = g_OnScopeExit > [&]
			{
				mp_VersionDownloads.f_Remove(DownloadID);
			}
		;
		
		CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Params.m_VersionID.f_EncodeFileName());
		
		Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(VersionPath);
		Download.m_Desc = fg_Format("{}", _Params.m_VersionID);
		
		TCContinuation<CVersionManager::CStartDownloadVersion::CResult> Continuation;
		
		Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Move(_Params.m_TransferContext)) 
			> [this, _CallingHostInfo, DownloadID, Continuation, Desc = Download.m_Desc](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				if (!_Subscription)
				{
					fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to initialize version download: {}", Desc, _Subscription.f_GetExceptionStr()));
					Continuation.f_SetException(DMibErrorInstance("Failed to send files. Consult version manager log files for more info."));
					return;
				}
				CVersionManager::CStartDownloadVersion::CResult Result;
				Result.m_Subscription = fg_Move(fg_Move(*_Subscription));
				Continuation.f_SetResult(fg_Move(Result));
				auto *pDownload = mp_VersionDownloads.f_FindEqual(DownloadID);
				if (!pDownload)
					return;
				auto &Download = *pDownload;
				Download.m_FileTransferSend(&CFileTransferSend::f_GetResult) > [this, _CallingHostInfo, DownloadID, Desc](TCAsyncResult<CFileTransferResult> &&_Result)
					{
						if (!_Result)
							fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to transfer version (download): {}", Desc, _Result.f_GetExceptionStr()));
						else
						{
							auto &Result = *_Result;
							CStr Message;
							Message = fg_Format("{} bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);
							
							fsp_LogActivityInfo
								(
									_CallingHostInfo
									, fg_Format
									(
										"'{}' Version download finished transferring: {}"
										, Desc
										, Message
									)
								)
							;
						}
						
						auto *pDownload = mp_VersionDownloads.f_FindEqual(DownloadID);
						if (!pDownload)
							return;
						if (pDownload->m_FileTransferSend)
						{
							pDownload->m_FileTransferSend->f_Destroy();
							pDownload->m_FileTransferSend.f_Clear();
						}
						mp_VersionDownloads.f_Remove(DownloadID);
					}
				;
			}
		;
		
		Cleanup.f_Clear();
		
		fsp_LogActivityInfo(_CallingHostInfo, fg_Format("'{}' Starting download of version", Download.m_Desc));
		
		return Continuation;
	}
}

