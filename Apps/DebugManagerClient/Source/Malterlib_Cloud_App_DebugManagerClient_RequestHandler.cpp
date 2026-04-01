// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_DebugManagerClient.h"

#include <Mib/Web/HTTPServer>
#include <Mib/Compression/ZstandardAsync>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Concurrency/LogError>
#include <Mib/Network/ResolveActor>

namespace NMib::NCloud::NDebugManagerClient
{
	TCFuture<bool> CDebugManagerClientApp::fp_HandleRequest(NStorage::TCSharedPointer<CHTTPConnection> _pConnection, NStorage::TCSharedPointer<CHTTPRequest> _pRequest)
	{
		auto fRespondNotFound = [&](CStr const &_Message)
			{
				CHTTPResponseHeader ResponseHeader;
				ResponseHeader.m_CacheControl = "no-store";
				ResponseHeader.m_LastModified = NTime::CTime::fs_NowUTC();
				ResponseHeader.m_Status = 404;

				_pConnection->f_WriteHeader(ResponseHeader);
				_pConnection->f_WriteStr("Not found ({})");

				return true;
			}
		;

		NHTTP::CURL RequestURL;
		RequestURL.f_Decode(_pRequest->m_RequestedURI);

		CStr BuildID;
		CStr Method;

		auto Result = (CStr::CParse("/buildid/{}/{}") >> BuildID >> Method).f_Execute(_pRequest->m_RequestedURI);

		if (Result.m_nVariablesParsed != 2)
			co_return fRespondNotFound("Unexpected path requested");

		CDebugManager::CAssetList ListParams;
		CDebugManager::CAssetFilter DownloadFilter;

		ListParams.m_Filter.m_BuildID = BuildID;
		DownloadFilter.m_BuildID = BuildID;

		if (Method == "debuginfo")
			ListParams.m_Filter.m_AssetType = CDebugManager::EAssetType::mc_DebugInfo;
		else if (Method == "executable")
			ListParams.m_Filter.m_AssetType = CDebugManager::EAssetType::mc_Executable;
		else
			co_return fRespondNotFound("{}: Only debuginfo and executable is supported"_f << BuildID);

		if (mp_DebugManagers.m_Actors.f_IsEmpty())
			co_return fRespondNotFound("{} {}: No DebugManager found to service the request"_f << BuildID << Method);

		for (auto &DebugManager : mp_DebugManagers.m_Actors)
		{
			auto ListResult = co_await DebugManager.m_Actor.f_CallActor(&CDebugManager::f_Asset_List)(ListParams).f_Wrap();
			if (!ListResult)
			{
				DMibLog(Warning, "({}) Failed to list assets: {}", DebugManager.m_TrustInfo.m_HostInfo, ListResult.f_GetExceptionStr());
				continue;
			}

			TCVector<CStr> FoundAssets;
			TCSet<CHashDigest_SHA256> FoundDigests;
			uint64 AssetSize = 0;
			CStr MainAssetFile;

			for (auto iAsset = co_await fg_Move(ListResult->m_AssetsGenerator).f_GetPipelinedIterator(); iAsset; co_await ++iAsset)
			{
				auto &&AssetVector = fg_Move(*iAsset);

				for (auto &Asset : AssetVector)
				{
					if (!FoundDigests(Asset.m_FileInfo.m_Digest).f_WasCreated())
						continue;

					if (FoundAssets.f_IsEmpty())
					{
						DownloadFilter.m_FileName = Asset.m_FileInfo.m_FileName;
						DownloadFilter.m_Metadata = Asset.m_Metadata;
						AssetSize = Asset.m_FileInfo.m_Size;
						if (Asset.m_MainAssetFile)
							MainAssetFile = Asset.m_FileInfo.m_FileName / Asset.m_MainAssetFile;
						else
							MainAssetFile = Asset.m_FileInfo.m_FileName;
					}

					FoundAssets.f_Insert(Asset.m_FileInfo.m_FileName);
				}
			}

			if (FoundAssets.f_IsEmpty())
				continue;

			if (FoundAssets.f_GetLen() > 1)
				DMibLog(Warning, "({}) Found several assets matching build ID '{}' {}: {vs} {}", DebugManager.m_TrustInfo.m_HostInfo, BuildID, Method, FoundAssets, MainAssetFile);
			else
				DMibLog(Info, "({}) Found asset matching build ID '{}' {}: {}", DebugManager.m_TrustInfo.m_HostInfo, BuildID, Method, MainAssetFile);

			CDebugManager::CAssetDownload DownloadParams;
			DownloadParams.m_Filter = DownloadFilter;
			DownloadParams.m_Subscription = g_ActorSubscription / [] -> TCFuture<void>
				{
					co_return {};
				}
			;

			auto DownloadResult = co_await DebugManager.m_Actor.f_CallActor(&CDebugManager::f_Asset_Download)(fg_Move(DownloadParams)).f_Wrap();
			if (!DownloadResult)
			{
				DMibLog(Warning, "({}) Failed to download asset: {}", DebugManager.m_TrustInfo.m_HostInfo, DownloadResult.f_GetExceptionStr());
				continue;
			}

			TCVector<CStr> DownloadableFiles;
			bool bSentFile = false;
			{
				auto iFile = co_await fg_Move(DownloadResult->m_FilesGenerator).f_GetPipelinedIterator();
				auto DestroyIterator = co_await NConcurrency::fg_AsyncDestroy(iFile);

				for (; iFile; co_await ++iFile)
				{
					if (bSentFile)
						continue;

					auto &&File = *iFile;

					if (File.m_FileAttributes & (EFileAttrib_Link | EFileAttrib_Directory))
						continue;

					DownloadableFiles.f_Insert(File.m_FilePath);

					if (File.m_FilePath != MainAssetFile)
						continue;

					auto DataGenerator = co_await File.m_fGetDataGenerator(0, fg_Default()).f_Wrap();
					if (!DataGenerator)
					{
						DMibLog(Warning, "({}) Failed to download asset (Get data generator): {}", DebugManager.m_TrustInfo.m_HostInfo, DataGenerator.f_GetExceptionStr());
						continue;
					}

					auto UncompressedGenerator = fg_DecompressZstandardAsync(fg_Move(DataGenerator->m_DataGenerator));

					CHTTPResponseHeader ResponseHeader;
					ResponseHeader.m_CacheControl = "no-store";
					ResponseHeader.m_LastModified = File.m_WriteTime;
					ResponseHeader.m_ContentLength = AssetSize;
					ResponseHeader.m_Status = 200;
					ResponseHeader.m_MimeType = "application/octet-stream";

					NConcurrency::TCFutureQueue<void> QueuedWrites(16);

					bSentFile = true;

					if (auto Future = QueuedWrites.f_Insert(_pConnection->f_WriteAsyncHeader(ResponseHeader)); Future.f_IsValid())
						co_await fg_Move(Future);

					for (auto iData = co_await fg_Move(UncompressedGenerator).f_GetPipelinedIterator(); iData; co_await ++iData)
					{
						if (auto Future = QueuedWrites.f_Insert(_pConnection->f_WriteAsyncBinary(fg_Move(*iData))); Future.f_IsValid())
							co_await fg_Move(Future);
					}

					while (QueuedWrites)
						co_await QueuedWrites.f_PopFirst();
				}
			}

			co_await g_AsyncDestroy;

			if (DownloadResult->m_Subscription)
				(void)co_await DownloadResult->m_Subscription->f_Destroy().f_Wrap();

			if (bSentFile)
				co_return true;

			DMibLog(Warning, "({}) Main asset '{}' not found in downloadable files: {}", DebugManager.m_TrustInfo.m_HostInfo, MainAssetFile, DownloadableFiles);
		}

		co_return fRespondNotFound("{} {}: Not found on any of the connected DebugManagers"_f << BuildID << Method);
	}
}
