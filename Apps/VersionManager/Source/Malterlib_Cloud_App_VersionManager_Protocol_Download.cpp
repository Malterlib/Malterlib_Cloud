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

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_DownloadVersion(CStartDownloadVersion &&_Params) -> TCContinuation<CStartDownloadVersion::CResult>
	{
		auto &CallingHostInfo = fg_GetCallingHostInfo();
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
		{
			CStr Error = "Invalid application format";
			fsp_LogActivityError(CallingHostInfo, Error + " (start download version)");
			return DMibErrorInstance(Error);
		}

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionIDAndPlatform.m_VersionID, ErrorStr))
			{
				CStr Error = fg_Format("Invalid version ID format: {}", ErrorStr);
				fsp_LogActivityError(CallingHostInfo, Error + " (start download version)");
				return DMibErrorInstance(Error);
			}
		}
		if (!CVersionManager::fs_IsValidPlatform(_Params.m_VersionIDAndPlatform.m_Platform))
		{
			CStr Error = fg_Format("Invalid version platform format");
			fsp_LogActivityError(CallingHostInfo, Error + " (start download version)");
			return DMibErrorInstance(Error);
		}
		
		bool bFullAccess = pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostInfo.f_GetRealHostID(), "Application/ReadAll");
		
		while (!bFullAccess)
		{
			if (pThis->mp_Permissions.f_HostHasPermission(CallingHostInfo.f_GetRealHostID(), fg_Format("Application/Read/{}", _Params.m_Application)))
				break;
			
			return pThis->fp_AccessDenied(CallingHostInfo, "Start download version");
		}
		
		auto *pApplication = pThis->mp_Applications.f_FindEqual(_Params.m_Application);
		if (!pApplication)
		{
			CStr Error = fg_Format("No such application: {}", _Params.m_Application);
			fsp_LogActivityError(CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		auto *pVersion = pApplication->m_Versions.f_FindEqual(_Params.m_VersionIDAndPlatform);
		if (!pVersion)
		{
			CStr Error = fg_Format("No such version: {}", _Params.m_VersionIDAndPlatform);
			fsp_LogActivityError(CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		
		NStr::CStr DownloadID = fg_RandomID();
		
		auto &Download = pThis->mp_VersionDownloads[DownloadID];
		auto Cleanup = g_OnScopeExit > [&]
			{
				pThis->mp_VersionDownloads.f_Remove(DownloadID);
			}
		;
		
		CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Params.m_VersionIDAndPlatform.f_EncodeFileName());
		
		Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(VersionPath);
		Download.m_Desc = fg_Format("{}", _Params.m_VersionIDAndPlatform);
		
		TCContinuation<CVersionManager::CStartDownloadVersion::CResult> Continuation;
		
		Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Move(_Params.m_TransferContext)) 
			> [pThis, CallingHostInfo, DownloadID, Continuation, Desc = Download.m_Desc, VersionInfo = pVersion->m_VersionInfo](TCAsyncResult<CActorSubscription> &&_Subscription) mutable
			{
				if (!_Subscription)
				{
					fsp_LogActivityError(CallingHostInfo, fg_Format("'{}' Failed to initialize version download: {}", Desc, _Subscription.f_GetExceptionStr()));
					Continuation.f_SetException(DMibErrorInstance("Failed to send files. Consult version manager log files for more info."));
					return;
				}
				CVersionManager::CStartDownloadVersion::CResult Result;
				Result.m_Subscription = fg_Move(fg_Move(*_Subscription));
				Result.m_VersionInfo = fg_Move(VersionInfo);
				Continuation.f_SetResult(fg_Move(Result));
				auto *pDownload = pThis->mp_VersionDownloads.f_FindEqual(DownloadID);
				if (!pDownload)
					return;
				auto &Download = *pDownload;
				Download.m_FileTransferSend(&CFileTransferSend::f_GetResult) > [pThis, CallingHostInfo, DownloadID, Desc](TCAsyncResult<CFileTransferResult> &&_Result)
					{
						if (!_Result)
							fsp_LogActivityError(CallingHostInfo, fg_Format("'{}' Failed to transfer version (download): {}", Desc, _Result.f_GetExceptionStr()));
						else
						{
							auto &Result = *_Result;
							CStr Message;
							Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);
							
							fsp_LogActivityInfo
								(
									CallingHostInfo
									, fg_Format
									(
										"'{}' Version download finished transferring: {}"
										, Desc
										, Message
									)
								)
							;
						}
						
						auto *pDownload = pThis->mp_VersionDownloads.f_FindEqual(DownloadID);
						if (!pDownload)
							return;
						if (pDownload->m_FileTransferSend)
						{
							pDownload->m_FileTransferSend->f_Destroy();
							pDownload->m_FileTransferSend.f_Clear();
						}
						pThis->mp_VersionDownloads.f_Remove(DownloadID);
					}
				;
			}
		;
		
		Cleanup.f_Clear();
		
		fsp_LogActivityInfo(CallingHostInfo, fg_Format("'{}' Starting download of version", Download.m_Desc));
		
		return Continuation;
	}
}

