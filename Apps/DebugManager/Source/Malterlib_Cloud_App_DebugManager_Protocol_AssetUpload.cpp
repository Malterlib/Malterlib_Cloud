// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/UUID>

#include "Malterlib_Cloud_App_DebugManager.h"
#include "Malterlib_Cloud_App_DebugManager_Protocol_Conversion.hpp"

namespace NMib::NCloud::NDebugManager
{
	auto CDebugManagerApp::CDebugManagerImplementation::f_Asset_Upload(CAssetUpload _Params) -> TCFuture<CAssetUpload::CResult>
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();
		auto Auditor = pThis->mp_State.f_Auditor({}, CallingHostInfo);

		if (_Params.m_QueueSize > mcp_MaxQueueSize)
			co_return Auditor.f_Exception("Queue size larger than maximum allowed");

		TCVector<CStr> Permissions{"DebugManager/WriteAll", "DebugManager/WriteAsset"};

		TCSharedPointer<CAssetUpload> pParams = fg_Construct(fg_Move(_Params));

		auto bHasPermissions = co_await (pThis->mp_Permissions.f_HasPermission("Upload asset", Permissions) % "Permission denied uploading asset" % Auditor);
		if (!bHasPermissions)
			co_return Auditor.f_AccessDenied("(Upload asset)", Permissions);

		CStr UploadID = fg_RandomID(pThis->mp_Uploads);
		pThis->mp_Uploads[UploadID];

		CUpload *pUpload = nullptr;
		auto OnResume2 = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pUpload = pThis->mp_Uploads.f_FindEqual(UploadID);
					if (!pUpload)
						return DMibErrorInstance("Upload aborted");

					return {};
				}
			)
		;

		auto Cleanup = g_ActorSubscription / [pThis, UploadID, Auditor]() -> TCFuture<void>
			{
				if (pThis->mp_Uploads.f_Remove(UploadID))
					Auditor.f_Error("Aborted upload of asset");

				co_return {};
			}
		;

		CStr UploadPath = pThis->mp_DebugDatabaseInitResult.m_TempDirectory / fg_RandomID();

		auto CleanupTemp = NConcurrency::g_BlockingActorSubscription / [UploadPath]
			{
				if (NFile::CFile::fs_FileExists(UploadPath))
					NFile::CFile::fs_DeleteDirectoryRecursive(UploadPath);
			}
		;

		pUpload->m_FileTransferReceive = fg_ConstructActor<CFileTransferReceive>
			(
				UploadPath
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_UnixAttributesValid
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
			)
		;

		auto ReceiveFlags = CFileTransferReceive::EReceiveFlag_FailOnExisting;

		if (!pParams->m_FilesGenerator.f_IsValid())
			co_return Auditor.f_Exception("Internal error: Files generator missing");

		auto ReceiveFilesFuture = pUpload->m_FileTransferReceive
			(
				&CFileTransferReceive::f_ReceiveFiles
				, CFileTransferSendDownloadFile::fs_TranslateGenerator<CFileTransferSendDownloadFile>(fg_Move(pParams->m_FilesGenerator))
				, pParams->m_QueueSize
				, ReceiveFlags
			)
			.f_Call()
		;

		TCSharedPointer<TCPromise<void>> pFinishedPromise = fg_Construct();

		CDebugManager::CAssetUpload::CResult UploadResult;

		UploadResult.m_fFinish = g_ActorFunctor
			(
				g_ActorSubscription / [pThis, UploadID, Auditor]() -> TCFuture<void>
				{
					auto *pUpload = pThis->mp_Uploads.f_FindEqual(UploadID);
					if (!pUpload)
						co_return {};

					TCFutureVector<void> DestroyResults;

					if (pUpload->m_FileTransferReceive)
						fg_Move(pUpload->m_FileTransferReceive).f_Destroy() > DestroyResults;

					if (pThis->mp_Uploads.f_Remove(UploadID))
						Auditor.f_Error("Aborted upload of asset");

					co_await fg_AllDone(DestroyResults).f_Wrap() > fg_LogWarning("AssetUpload", "Failed to destroy asset upload");

					co_return {};
				}
			)
			/ [Future = COptionalFuture{pFinishedPromise->f_Future()}, AllowDestroy = g_AllowWrongThreadDestroy]() mutable -> TCFuture<void>
			{
				return fg_Move(Future.m_Future);
			}
		;

		g_Dispatch /
			[
				pThis
				, UploadID
				, Auditor
				, pParams
				, pFinishedPromise
				, Cleanup = fg_Move(Cleanup)
				, ReceiveFilesFuture = fg_Move(ReceiveFilesFuture)
				, UploadPath
				, CleanupTemp = fg_Move(CleanupTemp)
			]
			() mutable -> TCFuture<void>
			{
				auto ReceiveResult = co_await fg_Move(ReceiveFilesFuture).f_Wrap();
				if (!ReceiveResult)
					Auditor.f_Error("Failed to transfer asset (upload): {}"_f << ReceiveResult.f_GetExceptionStr());
				else
				{
					auto &Result = *ReceiveResult;
					CStr Message;
					Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);

					Auditor.f_Info("Debug asset upload finished transferring: {}"_f << Message);
				}

				auto *pUpload = pThis->mp_Uploads.f_FindEqual(UploadID);
				if (!pUpload)
				{
					pFinishedPromise->f_SetException(DMibErrorInstance("Upload aborted"));
					co_return {};
				}

				if (pUpload->m_FileTransferReceive)
					fg_Move(pUpload->m_FileTransferReceive).f_Destroy().f_DiscardResult();

				pThis->mp_Uploads.f_Remove(UploadID);

				if (!ReceiveResult)
				{
					pFinishedPromise->f_SetException(Auditor.f_Exception("Failed to receive files. See Debug Manager log."));

					co_return {};
				}

				struct CRootFile
				{
					CStr m_FileName;
					CStr m_Path;
				};

				TCVector<CRootFile> RootFiles;
				{
					auto BlockingActorCheckout = NConcurrency::fg_BlockingActor();

					auto RootFilesResult = co_await
						(
							NConcurrency::g_Dispatch(BlockingActorCheckout) / [UploadPath]() -> NConcurrency::TCFuture<TCVector<CRootFile>>
							{
								auto CaptureExceptions = co_await (NConcurrency::g_CaptureExceptions % "Error trying examine upload results");

								NFile::CFile::CFindFilesOptions FindFilesOptions(UploadPath / "*", false);
								FindFilesOptions.m_AttribMask = NFile::EFileAttrib_File | NFile::EFileAttrib_Directory;

								auto Files = NFile::CFile::fs_FindFiles(FindFilesOptions);
								Files.f_Sort();

								TCVector<CRootFile> RootFiles;

								for (auto &File : Files)
								{
									CStr RootPath = UploadPath;
									CStr RelativePath = File.m_Path;

									auto CommonPath = NFile::CFile::fs_GetCommonPathAndMakeRelative(RootPath, RelativePath);

									if (!RootPath.f_IsEmpty())
										co_return DMibErrorInstance("Internal error, could not determine relative path: {}"_f << RootPath);

									RootFiles.f_Insert
										(
											CRootFile
											{
												.m_FileName = RelativePath
												, .m_Path = File.m_Path
											}
										)
									;
								}

								co_return fg_Move(RootFiles);
							}
						)
						.f_Wrap()
					;

					if (!RootFilesResult)
					{
						pFinishedPromise->f_SetException(DMibErrorInstance("Failed to add asset to debug database. See Debug Manager log."));
						Auditor.f_Error("Failed to find root files in asset upload: {}"_f << RootFilesResult.f_GetExceptionStr());
						co_return {};
					}

					RootFiles = *RootFilesResult;
				}

				for (auto &RootFile : RootFiles)
				{
					auto AssetAdd = fg_ConvertToDebugDatabase<CDebugDatabase::CAssetAdd>(fg_Const(*pParams));
					AssetAdd.m_FileName = RootFile.m_FileName;
					AssetAdd.m_Path = RootFile.m_Path;

					auto AssetAddResult = co_await pThis->mp_DebugDatabase
						(
							&CDebugDatabase::f_Asset_Add
							, fg_Move(AssetAdd)
						)
						.f_Wrap()
					;

					if (!AssetAddResult)
					{
						CStr ErrorMessage = "Failed to add asset to debug database: {}"_f << AssetAddResult.f_GetExceptionStr();
						pFinishedPromise->f_SetException(DMibErrorInstance(ErrorMessage));
						Auditor.f_Error(ErrorMessage);

						co_return {};
					}
				}

				pFinishedPromise->f_SetResult();

				co_return {};
			}
			> fg_LogError("AssetUpload", "Failed to transfer asset (upload)");
		;

		Auditor.f_Info("Starting upload of asset");

		co_return fg_Move(UploadResult);
	}
}
