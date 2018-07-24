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
			m_FileTransferSend->f_DestroyNoResult(DMibPFile, DMibPLine);
			m_FileTransferSend.f_Clear();
		}
	}

	CVersionManagerDaemonActor::CServer::CVersionDownload::CVersionDownload()
	{
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_DownloadVersion(CStartDownloadVersion &&_Params) -> TCContinuation<CStartDownloadVersion::CResult>
	{
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor(); 
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
			return Auditor.f_Exception({"Invalid application format", "(start download version)"});

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionIDAndPlatform.m_VersionID, ErrorStr))
				return Auditor.f_Exception({fg_Format("Invalid version ID format: {}", ErrorStr), "(start download version)"});
		}
		if (!CVersionManager::fs_IsValidPlatform(_Params.m_VersionIDAndPlatform.m_Platform))
			return Auditor.f_Exception({"Invalid version platform format", "(start download version)"});

		TCContinuation<CStartDownloadVersion::CResult> Continuation;
		pThis->mp_Permissions.f_HasPermission("Download version from VersionManager", {"Application/ReadAll", "Application/Read/{}"_f << _Params.m_Application})
			> Continuation % "Permission denied downloading version" % Auditor /
			[=, Params = fg_Move(_Params)](bool _bHasPermission) mutable
			{
				if (!_bHasPermission)
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Start download version)"));

				auto *pApplication = pThis->mp_Applications.f_FindEqual(Params.m_Application);
				if (!pApplication)
					return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No such application: {}", Params.m_Application)));
				auto *pVersion = pApplication->m_Versions.f_FindEqual(Params.m_VersionIDAndPlatform);
				if (!pVersion)
					return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No such version: {}", Params.m_VersionIDAndPlatform)));

				NStr::CStr DownloadID = fg_RandomID();

				auto &Download = pThis->mp_VersionDownloads[DownloadID];
				auto Cleanup = g_OnScopeExit > [&]
					{
						pThis->mp_VersionDownloads.f_Remove(DownloadID);
					}
				;

				CStr ApplicationDirectory = fg_Format("{}/Applications", pThis->mp_AppState.m_RootDirectory);
				CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, Params.m_Application, Params.m_VersionIDAndPlatform.f_EncodeFileName());

				Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(VersionPath);
				Download.m_Desc = fg_Format("{}", Params.m_VersionIDAndPlatform);

				Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Move(Params.m_TransferContext))
					> [pThis, DownloadID, Continuation, Desc = Download.m_Desc, VersionInfo = pVersion->m_VersionInfo, Auditor]
					(TCAsyncResult<CActorSubscription> &&_Subscription) mutable
					{
						if (!_Subscription)
						{
							Continuation.f_SetException
								(
									Auditor.f_Exception
									(
										{
											fg_Format("'{}' Failed to send files. Check Version Manager log for more info.", Desc)
											, fg_Format("Error: {}", _Subscription.f_GetExceptionStr())
										}
									)
								)
							;
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
						Download.m_FileTransferSend(&CFileTransferSend::f_GetResult) > [pThis, DownloadID, Desc, Auditor](TCAsyncResult<CFileTransferResult> &&_Result)
							{
								if (!_Result)
									Auditor.f_Error(fg_Format("'{}' Failed to transfer version (download): {}", Desc, _Result.f_GetExceptionStr()));
								else
								{
									auto &Result = *_Result;
									CStr Message;
									Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);

									Auditor.f_Info
										(
											fg_Format
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
									pDownload->m_FileTransferSend->f_DestroyNoResult(DMibPFile, DMibPLine);
									pDownload->m_FileTransferSend.f_Clear();
								}
								pThis->mp_VersionDownloads.f_Remove(DownloadID);
							}
						;
					}
				;

				Cleanup.f_Clear();

				Auditor.f_Info(fg_Format("'{}' Starting download of version", Download.m_Desc));
			}
		;

		return Continuation;
	}
}

