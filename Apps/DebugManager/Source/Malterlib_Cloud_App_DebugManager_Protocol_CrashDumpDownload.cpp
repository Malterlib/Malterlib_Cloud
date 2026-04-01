// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_CrashDump_Download(CCrashDumpDownload _Params) -> TCFuture<CCrashDumpDownload::CResult>
	{
		auto pThis = m_pThis;
		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		TCSharedPointer<CCrashDumpDownload> pParams = fg_Construct(fg_Move(_Params));

		TCVector<CStr> Permissions{"DebugManager/ReadAll", "DebugManager/ReadCrashDump"};

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("Download crash dump", Permissions) % "Permission denied downloading crash dump" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Download crash dump)", Permissions);

		auto DownloadID = fg_RandomID(pThis->mp_Downloads);
		auto &Download = pThis->mp_Downloads[DownloadID];

		auto Cleanup = g_OnScopeExit / [&]
			{
				if (pParams->m_Subscription)
					pParams->m_Subscription->f_Destroy().f_DiscardResult();

				pThis->mp_Downloads.f_Remove(DownloadID);
			}
		;

		auto CrashDumpGenerator = co_await
			(
				pThis->mp_DebugDatabase
				(
					&CDebugDatabase::f_CrashDump_List
					, fg_ConvertToDebugDatabase<CDebugDatabase::CCrashDumpFilter>(fg_Move(pParams->m_Filter))
				)
				% "Failed to list crash dumps when downloading" % Auditor
			)
		;

		{
			struct CCrashDumpInfo
			{
				CFileTransferSend::CBasePath m_BasePath;
				CStr m_ID;

			};

			struct CUniqueDownload
			{
				TCSet<CHashDigest_SHA256> m_AddedCrashDumps;
				TCVector<CCrashDumpInfo> m_CrashDumpInfos;
			};

			TCMap<CStr, CUniqueDownload> UniqueDownloads;

			bool bNeedDisambiguation = false;

			for (auto iCrashDump = co_await fg_Move(CrashDumpGenerator).f_GetPipelinedIterator(); iCrashDump; co_await ++iCrashDump)
			{
				for (auto &CrashDump : *iCrashDump)
				{
					auto &UniqueDownload = UniqueDownloads[CrashDump.m_FileInfo.m_FileName];

					if (!UniqueDownload.m_AddedCrashDumps(CrashDump.m_FileInfo.m_Digest).f_WasCreated())
						continue;

					UniqueDownload.m_CrashDumpInfos.f_Insert
						(
							CCrashDumpInfo{.m_BasePath = {.m_Path = CrashDump.m_StoredPath, .m_Name = CrashDump.m_FileInfo.m_FileName}, .m_ID = CrashDump.m_ID}
						)
					;

					if (UniqueDownload.m_CrashDumpInfos.f_GetLen() > 1)
						bNeedDisambiguation = true;
				}
			}

			TCVector<CFileTransferSend::CBasePath> CrashDumpFiles;
			for (auto &UniqueDownload : UniqueDownloads)
			{
				for (auto &CrashDumpInfo : UniqueDownload.m_CrashDumpInfos)
				{
					if (bNeedDisambiguation)
						CrashDumpFiles.f_Insert(CFileTransferSend::CBasePath{.m_Path = CrashDumpInfo.m_BasePath.m_Path, .m_Name = CrashDumpInfo.m_ID / CrashDumpInfo.m_BasePath.m_Name});
					else
						CrashDumpFiles.f_Insert(fg_Move(CrashDumpInfo.m_BasePath));
				}
			}

			if (CrashDumpFiles.f_IsEmpty())
				co_return DMibErrorInstance("No crash dumps found");

			Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(fg_Move(CrashDumpFiles));
		}

		Cleanup.f_Clear();
		Auditor.f_Info(fg_Format("Starting download of crash dumps"));

		auto SendResult = co_await Download.m_FileTransferSend(&CFileTransferSend::f_SendFiles, fg_Default()).f_Wrap();
		if (!SendResult)
		{
			co_return Auditor.f_Exception
				(
					{
						"Failed to send files. Check Debug Manager log for more info."
						, "Error: {}"_f << SendResult.f_GetExceptionStr()
					}
				)
			;
		}

		CCrashDumpDownload::CResult Result;
		Result.m_Subscription = fg_Move(SendResult->m_Subscription);
		Result.m_FilesGenerator = CFileTransferSendDownloadFile::fs_TranslateGenerator<CDebugManager::CDownloadFile>(fg_Move(SendResult->m_FilesGenerator));
		auto *pDownload = pThis->mp_Downloads.f_FindEqual(DownloadID);
		if (!pDownload)
			co_return DMibErrorInstance("Download aborted");

		fg_Move(SendResult->m_Result) > [pThis, DownloadID, Auditor, pParams](TCAsyncResult<CFileTransferResult> &&_Result)
			{
				if (!_Result)
					Auditor.f_Error("Failed to transfer crash dump (download): {}"_f << _Result.f_GetExceptionStr());
				else
				{
					auto &Result = *_Result;
					CStr Message;
					Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);

					Auditor.f_Info("Crash dumps download finished transferring: {}"_f << Message);
				}

				auto *pDownload = pThis->mp_Downloads.f_FindEqual(DownloadID);
				if (!pDownload)
					return;

				if (pDownload->m_FileTransferSend)
					fg_Move(pDownload->m_FileTransferSend).f_Destroy().f_DiscardResult();

				pThis->mp_Downloads.f_Remove(DownloadID);
			}
		;

		co_return fg_Move(Result);
	}
}
