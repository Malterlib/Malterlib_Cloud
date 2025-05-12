// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_Asset_Download(CAssetDownload _Params) -> TCFuture<CAssetDownload::CResult>
	{
		auto pThis = m_pThis;
		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		TCSharedPointer<CAssetDownload> pParams = fg_Construct(fg_Move(_Params));

		TCVector<CStr> Permissions{"DebugManager/ReadAll", "DebugManager/ReadAsset"};

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("Download asset", Permissions) % "Permission denied downloading asset" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Download asset)", Permissions);

		auto DownloadID = fg_RandomID(pThis->mp_Downloads);
		auto &Download = pThis->mp_Downloads[DownloadID];

		auto Cleanup = g_OnScopeExit / [&]
			{
				if (pParams->m_Subscription)
					pParams->m_Subscription->f_Destroy().f_DiscardResult();

				pThis->mp_Downloads.f_Remove(DownloadID);
			}
		;

		auto AssetGenerator = co_await
			(
				pThis->mp_DebugDatabase
				(
					&CDebugDatabase::f_Asset_List
					, fg_ConvertToDebugDatabase<CDebugDatabase::CAssetFilter>(fg_Move(pParams->m_Filter))
				)
				% "Failed to list assets when downloading" % Auditor
			)
		;

		{
			struct CAssetInfo
			{
				CFileTransferSend::CBasePath m_BasePath;
				CStr m_BuildID;

			};

			struct CUniqueDownload
			{
				TCSet<CHashDigest_SHA256> m_AddedAssets;
				TCVector<CAssetInfo> m_AssetInfos;
			};

			TCMap<CStr, CUniqueDownload> UniqueDownloads;

			bool bNeedDisambiguation = false;

			for (auto iAsset = co_await fg_Move(AssetGenerator).f_GetPipelinedIterator(); iAsset; co_await ++iAsset)
			{
				for (auto &Asset : *iAsset)
				{
					auto &UniqueDownload = UniqueDownloads[Asset.m_FileInfo.m_FileName];

					if (!UniqueDownload.m_AddedAssets(Asset.m_FileInfo.m_Digest).f_WasCreated())
						continue;

					UniqueDownload.m_AssetInfos.f_Insert(CAssetInfo{.m_BasePath = {.m_Path = Asset.m_StoredPath, .m_Name = Asset.m_FileInfo.m_FileName}, .m_BuildID = Asset.m_BuildID});

					if (UniqueDownload.m_AssetInfos.f_GetLen() > 1)
						bNeedDisambiguation = true;
				}
			}

			TCVector<CFileTransferSend::CBasePath> AssetFiles;
			for (auto &UniqueDownload : UniqueDownloads)
			{
				for (auto &AssetInfo : UniqueDownload.m_AssetInfos)
				{
					if (bNeedDisambiguation)
						AssetFiles.f_Insert(CFileTransferSend::CBasePath{.m_Path = AssetInfo.m_BasePath.m_Path, .m_Name = AssetInfo.m_BuildID / AssetInfo.m_BasePath.m_Name});
					else
						AssetFiles.f_Insert(fg_Move(AssetInfo.m_BasePath));
				}
			}

			if (AssetFiles.f_IsEmpty())
				co_return DMibErrorInstance("No assets found");

			Download.m_FileTransferSend = fg_ConstructActor<CFileTransferSend>(fg_Move(AssetFiles));
		}

		Cleanup.f_Clear();
		Auditor.f_Info(fg_Format("Starting download of assets"));

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

		CAssetDownload::CResult Result;
		Result.m_Subscription = fg_Move(SendResult->m_Subscription);
		Result.m_FilesGenerator = CFileTransferSendDownloadFile::fs_TranslateGenerator<CDebugManager::CDownloadFile>(fg_Move(SendResult->m_FilesGenerator));
		auto *pDownload = pThis->mp_Downloads.f_FindEqual(DownloadID);
		if (!pDownload)
			co_return DMibErrorInstance("Download aborted");

		fg_Move(SendResult->m_Result) > [pThis, DownloadID, Auditor, pParams](TCAsyncResult<CFileTransferResult> &&_Result)
			{
				if (!_Result)
					Auditor.f_Error("Failed to transfer asset (download): {}"_f << _Result.f_GetExceptionStr());
				else
				{
					auto &Result = *_Result;
					CStr Message;
					Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);

					Auditor.f_Info("Assets download finished transferring: {}"_f << Message);
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
