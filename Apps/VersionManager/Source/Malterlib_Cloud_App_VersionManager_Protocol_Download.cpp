// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CVersionDownload::~CVersionDownload()
	{
		if (m_FileTransferSend)
			fg_Move(m_FileTransferSend).f_Destroy().f_DiscardResult();
	}

	CVersionManagerDaemonActor::CServer::CVersionDownload::CVersionDownload()
	{
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_DownloadVersion(CStartDownloadVersion _Params) -> TCFuture<CStartDownloadVersion::CResult>
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		uint32 ProtocolVersion = CallingHostInfo.f_GetProtocolVersion();

		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor({}, CallingHostInfo);

		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
			co_return Auditor.f_Exception({"Invalid application format", "(start download version)"});

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionIDAndPlatform.m_VersionID, ErrorStr))
				co_return Auditor.f_Exception({fg_Format("Invalid version ID format: {}", ErrorStr), "(start download version)"});
		}

		if (!CVersionManager::fs_IsValidPlatform(_Params.m_VersionIDAndPlatform.m_Platform))
			co_return Auditor.f_Exception({"Invalid version platform format", "(start download version)"});

		TCSharedPointer<CStartDownloadVersion> pParams = fg_Construct(fg_Move(_Params));

		NContainer::TCVector<NStr::CStr> Permissions = {"Application/ReadAll", "Application/Read/{}"_f << pParams->m_Application};

		bool bHasPermission = co_await
			(
				pThis->mp_Permissions.f_HasPermission("Download version from VersionManager", Permissions)
				% "Permission denied downloading version"
				% Auditor
			)
		;

		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Start download version)", Permissions);

		auto *pApplication = pThis->mp_Applications.f_FindEqual(pParams->m_Application);
		if (!pApplication)
			co_return Auditor.f_Exception(fg_Format("No such application: {}", pParams->m_Application));
		auto *pVersion = pApplication->m_Versions.f_FindEqual(pParams->m_VersionIDAndPlatform);
		if (!pVersion)
			co_return Auditor.f_Exception(fg_Format("No such version: {}", pParams->m_VersionIDAndPlatform));

		NStr::CStr DownloadID = fg_RandomID(pThis->mp_VersionDownloads);

		auto &Download = pThis->mp_VersionDownloads[DownloadID];
		auto Cleanup = g_OnScopeExit / [&]
			{
				pThis->mp_VersionDownloads.f_Remove(DownloadID);
			}
		;

		CStr ApplicationDirectory = fg_Format("{}/Applications", pThis->mp_AppState.m_RootDirectory);
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, pParams->m_Application, pParams->m_VersionIDAndPlatform.f_EncodeFileName());

		Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(VersionPath);
		Download.m_Desc = fg_Format("{}", pParams->m_VersionIDAndPlatform);

		Cleanup.f_Clear();
		Auditor.f_Info(fg_Format("'{}' Starting download of version", Download.m_Desc));

		auto VersionInfo = pVersion->m_VersionInfo;

		if (ProtocolVersion >= CVersionManager::EProtocolVersion_AsyncGeneratorFileTransfer)
		{
			auto SendResult = co_await Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Default()).f_Wrap();
			if (!SendResult)
			{
				co_return Auditor.f_Exception
					(
						{
							fg_Format("'{}' Failed to send files. Check Version Manager log for more info.", Download.m_Desc)
							, fg_Format("Error: {}", SendResult.f_GetExceptionStr())
						}
					)
				;
			}

			CVersionManager::CStartDownloadVersion::CResult Result;
			Result.m_Subscription = fg_Move(SendResult->m_Subscription);
			Result.m_VersionInfo = fg_Move(VersionInfo);
			Result.m_FilesGenerator = CFileTransferSendDownloadFile::fs_TranslateGenerator<CVersionManager::CDownloadFile>(fg_Move(SendResult->m_FilesGenerator));
			auto *pDownload = pThis->mp_VersionDownloads.f_FindEqual(DownloadID);
			if (!pDownload)
				co_return DMibErrorInstance("Download aborted");

			fg_Move(SendResult->m_Result) > [pThis, DownloadID, Desc = Download.m_Desc, Auditor, pParams](TCAsyncResult<CFileTransferResult> &&_Result)
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
						fg_Move(pDownload->m_FileTransferSend).f_Destroy().f_DiscardResult();

					pThis->mp_VersionDownloads.f_Remove(DownloadID);
				}
			;

			co_return fg_Move(Result);
		}
		else
		{
			if (!pParams->m_TransferContextDeprecated)
				co_return Auditor.f_Exception({"Internal error: Invalid transfer context", "(start download version)"});

			auto SendResult = co_await Download.m_FileTransferSend(&CFileTransferSend::f_SendFilesDeprecated, fg_Move(*(pParams->m_TransferContextDeprecated))).f_Wrap();
			if (!SendResult)
			{
				co_return Auditor.f_Exception
					(
						{
							fg_Format("'{}' Failed to send files. Check Version Manager log for more info.", Download.m_Desc)
							, fg_Format("Error: {}", SendResult.f_GetExceptionStr())
						}
					)
				;
			}

			CVersionManager::CStartDownloadVersion::CResult Result;
			Result.m_Subscription = fg_Move(SendResult->m_Subscription);
			Result.m_VersionInfo = fg_Move(VersionInfo);
			auto *pDownload = pThis->mp_VersionDownloads.f_FindEqual(DownloadID);
			if (!pDownload)
				co_return DMibErrorInstance("Download aborted");

			fg_Move(SendResult->m_Result) > [pThis, DownloadID, Desc = Download.m_Desc, Auditor, pParams](TCAsyncResult<CFileTransferResult> &&_Result)
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
						fg_Move(pDownload->m_FileTransferSend).f_Destroy().f_DiscardResult();

					pThis->mp_VersionDownloads.f_Remove(DownloadID);
				}
			;

			co_return fg_Move(Result);
		}
	}
}

