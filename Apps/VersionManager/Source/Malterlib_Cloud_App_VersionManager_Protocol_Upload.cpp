// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CVersionUpload::~CVersionUpload()
	{
		if (m_FileTransferReceive)
			fg_Move(m_FileTransferReceive).f_Destroy() > fg_DiscardResult();
	}

	CVersionManagerDaemonActor::CServer::CVersionUpload::CVersionUpload()
	{
	}

	static constexpr EFileAttrib gc_FilePermissions = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid;

	auto CVersionManagerDaemonActor::CServer::fp_SaveVersionInfo(TCActor<> const &_FileActor, CStr const &_VersionPath, CVersionManager::CVersionInformation const &_VersionInfo) 
		-> TCFuture<CSizeInfo>
	{
		TCPromise<CSizeInfo> Promise;
		g_Dispatch(_FileActor) / [_VersionInfo, _VersionPath]() mutable -> CSizeInfo
			{
				CEJSON VersionJSONInfo;
				CStr VersionInfoPath = fg_Format("{}.json", _VersionPath);

				VersionJSONInfo["Time"] = _VersionInfo.m_Time;
				VersionJSONInfo["Configuration"] = _VersionInfo.m_Configuration;
				if (_VersionInfo.m_ExtraInfo.f_IsObject())
					VersionJSONInfo["ExtraInfo"] = _VersionInfo.m_ExtraInfo;
				auto &TagsArray = VersionJSONInfo["Tags"].f_Array();
				for (auto &Tag : _VersionInfo.m_Tags)
					TagsArray.f_Insert(Tag);
				VersionJSONInfo["RetrySequence"] = _VersionInfo.m_RetrySequence;

				CSizeInfo SizeInfo;
				auto Files = CFile::fs_FindFiles(_VersionPath + "/*", EFileAttrib_File, true);
				SizeInfo.m_nFiles = Files.f_GetLen();
				for (auto &File : Files)
					SizeInfo.m_nBytes += CFile::fs_GetFileSize(File);

				CFile::fs_CreateDirectory(CFile::fs_GetPath(VersionInfoPath));
				CFile::fs_WriteStringToFile(VersionInfoPath, VersionJSONInfo.f_ToString(), false, gc_FilePermissions);

				return SizeInfo;
			}
			> Promise
		;
		return Promise.f_MoveFuture();
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_UploadVersion(CStartUploadVersion &&_Params) -> TCFuture<CStartUploadVersion::CResult>
	{
		auto pThis = m_pThis;
		
		if (pThis->f_IsDestroyed())
			co_return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();

		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
			co_return Auditor.f_Exception({"Invalid application format", "(start upload version)"});

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionIDAndPlatform.m_VersionID, ErrorStr))
				co_return Auditor.f_Exception({fg_Format("Invalid version ID format: {}", ErrorStr), "(start upload version)"});
		}
		
		if (!CVersionManager::fs_IsValidPlatform(_Params.m_VersionIDAndPlatform.m_Platform))
			co_return Auditor.f_Exception({"Invalid version platform format", "(start upload version)"});

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["//Command//"] = {{"Application/WriteAll", fg_Format("Application/Write/{}", _Params.m_Application)}};
		Permissions["//TagAll//"] = {{"Application/TagAll"}};

		TCSet<CStr> DeniedTags;
		for (auto &Tag : _Params.m_VersionInfo.m_Tags)
		{
			if (CVersionManager::fs_IsValidTag(Tag))
				Permissions[Tag] = {CPermissionQuery{fg_Format("Application/Tag/{}", Tag)}.f_Description("Access to tag {} in VersionManager"_f << Tag)};
			else
				DeniedTags[Tag];
		}

		TCSharedPointer<CStartUploadVersion> pParams = fg_Construct(fg_Move(_Params));

		auto HasPermissions = co_await (pThis->mp_Permissions.f_HasPermissions("Upload version", Permissions) % "Permission denied uploading version" % Auditor);
		if (!HasPermissions["//Command//"])
			co_return Auditor.f_AccessDenied("(Start upload version)");

		auto VersionInfo = pParams->m_VersionInfo;

		if (!HasPermissions["//TagAll//"])
		{
			for (auto &Tag : VersionInfo.m_Tags)
			{
				auto pHasPermission = HasPermissions.f_FindEqual(Tag);
				if (!pHasPermission || !*pHasPermission)
					DeniedTags[Tag];
			}
			for (auto &Tag : DeniedTags)
				VersionInfo.m_Tags.f_Remove(Tag);
		}

		// Force time to same as when saving in JSON file
		VersionInfo.m_Time = CTimeConvert::fs_FromCreateFromUnixMilliseconds(CTimeConvert(VersionInfo.m_Time).f_UnixMilliseconds());

		CStr UploadID = fg_RandomID(pThis->mp_VersionUploads);

		{
			auto &Upload = pThis->mp_VersionUploads[UploadID];
			Upload.m_Desc = fg_Format("{} - {}", pParams->m_Application, pParams->m_VersionIDAndPlatform);
		}

		CVersionUpload *pUpload = nullptr;
		auto OnResume = g_OnResume / [&]
			{
				if (pThis->f_IsDestroyed())
					DMibError("Shutting down");

				pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
				if (!pUpload)
					DMibError("Upload aborted");
			}
		;

		auto pCleanup = g_OnScopeExitActor > [pThis, UploadID, Desc = pUpload->m_Desc, Auditor]
			{
				if (pThis->mp_VersionUploads.f_Remove(UploadID))
					Auditor.f_Error(fg_Format("'{}' Aborted upload of version", Desc));
			}
		;


		CStr ApplicationDirectory = fg_Format("{}/Applications", pThis->mp_AppState.m_RootDirectory);
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, pParams->m_Application, pParams->m_VersionIDAndPlatform.f_EncodeFileName());

		pUpload->m_UploadFileAccess = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Upload version file access"));
		pUpload->m_FileTransferReceive = fg_ConstructActor<CFileTransferReceive>(VersionPath, gc_FilePermissions, gc_FilePermissions, pUpload->m_UploadFileAccess);

		auto ReceiveFlags = CFileTransferReceive::EReceiveFlag_FailOnExisting;
		if (pParams->m_Flags & CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite)
			ReceiveFlags = CFileTransferReceive::EReceiveFlag_DeleteExisting;

		auto VersionID = pParams->m_VersionIDAndPlatform;
		auto ApplicationName = pParams->m_Application;

		auto Desc = pUpload->m_Desc;

		auto FileTransferContext = co_await pUpload->m_FileTransferReceive(&CFileTransferReceive::f_ReceiveFiles, pParams->m_QueueSize, ReceiveFlags).f_Wrap();
		if (!FileTransferContext)
		{
			CStr Error = FileTransferContext.f_GetExceptionStr();
			Auditor.f_Error(fg_Format("'{}' Failed to initialize version upload: {}", Desc, Error));
			if (Error == "Failed to generate current manifest: Directory already exists")
				co_return DMibErrorInstance("Version already exists");
			else
				co_return DMibErrorInstance("Failed to receive files. See Version Manager log.");
		}

		TCSharedPointer<TCPromise<void>> pFinishedPromise = fg_Construct();

		CVersionManager::CStartUploadTransfer StartTransfer;
		StartTransfer.m_TransferContext = fg_Move(*FileTransferContext);
		CVersionManager::CStartUploadVersion::CResult StartUploadResult;
		{
			auto Result = co_await pParams->m_fStartTransfer(fg_Move(StartTransfer)).f_Wrap();
			if (!Result)
			{
				CStr Error = Auditor.f_Error({"Failed to start transfer. See Version Manager log.", fg_Format("'{}': {}", Desc, Result.f_GetExceptionStr())});
				co_return DMibErrorInstance(Error);
			}

			pUpload->m_DownloadSubscription = fg_Move(Result->m_Subscription);
			StartUploadResult.m_DeniedTags = DeniedTags;
			StartUploadResult.m_fFinish = g_ActorFunctor
				(
					g_ActorSubscription / [pThis, UploadID, Desc, Auditor]() -> TCFuture<void>
					{
						auto *pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
						if (!pUpload)
							co_return {};

						if (pUpload->m_FileTransferReceive)
							co_await fg_Move(pUpload->m_FileTransferReceive).f_Destroy();

						if (pThis->mp_VersionUploads.f_Remove(UploadID))
							Auditor.f_Error(fg_Format("'{}' Aborted upload of version", Desc));

						co_return {};
					}
				)
				/ [pFinishedPromise]() -> TCFuture<void>
				{
					return pFinishedPromise->f_Future();
				}
			;
			pCleanup->f_Clear();
		}

		pUpload->m_FileTransferReceive(&CFileTransferReceive::f_GetResult)
			> [pThis, UploadID, ApplicationName, VersionID, VersionInfo, VersionPath, Desc, Auditor, pParams, pFinishedPromise]
			(TCAsyncResult<CFileTransferResult> &&_Result) mutable
			{
				if (!_Result)
					Auditor.f_Error(fg_Format("'{}' Failed to transfer version (upload): {}", Desc, _Result.f_GetExceptionStr()));
				else
				{
					auto &Result = *_Result;
					CStr Message;
					Message = fg_Format("{ns } bytes at {fe2} MB/s", Result.m_nBytes, Result.f_BytesPerSecond() / 1'000'000.0);

					Auditor.f_Info
						(
							fg_Format
							(
								"'{}' Version upload finished transferring: {}"
								, Desc
								, Message
							)
						)
					;
				}

				auto *pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
				if (!pUpload)
				{
					pFinishedPromise->f_SetException(DMibErrorInstance("Upload aborted"));
					return;
				}
				if (pUpload->m_FileTransferReceive)
					fg_Move(pUpload->m_FileTransferReceive).f_Destroy() > fg_DiscardResult();

				auto FileAccess = fg_Move(pUpload->m_UploadFileAccess);
				pThis->mp_VersionUploads.f_Remove(UploadID);

				if (!_Result)
				{
					pFinishedPromise->f_SetException(DMibErrorInstance("Upload file transfer failed"));
					return;
				}

				pThis->fp_SaveVersionInfo(FileAccess, VersionPath, VersionInfo)
					> [pThis, VersionID, VersionInfo, Desc, ApplicationName, Auditor, pParams, pFinishedPromise, FileAccess](TCAsyncResult<CSizeInfo> &&_InfoWriteResult) mutable
					{
						if (!_InfoWriteResult)
						{
#if DMibConfig_Tests_Enable
							pFinishedPromise->f_SetException(DMibErrorInstance("Failed to save version info: {}"_f << _InfoWriteResult.f_GetExceptionStr()));
#else
							pFinishedPromise->f_SetException(DMibErrorInstance("Failed to save version info"));
#endif
							Auditor.f_Error(fg_Format("'{}' Failed to write version info file: {}", Desc, _InfoWriteResult.f_GetExceptionStr()));
							return;
						}

						auto ApplicationMapped = pThis->mp_Applications(ApplicationName);
						auto &Application = *ApplicationMapped;

						if (ApplicationMapped.f_WasCreated())
						{
							TCSet<CStr> Permissions;
							{
								Permissions[fg_Format("Application/Read/{}", ApplicationName)];
								Permissions[fg_Format("Application/Write/{}", ApplicationName)];
							}
							pThis->mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
						}

						VersionInfo.m_nFiles  = _InfoWriteResult->m_nFiles;
						VersionInfo.m_nBytes = _InfoWriteResult->m_nBytes;

						auto &Version = Application.m_Versions[VersionID];
						if (Version.m_TimeLink.f_IsInTree())
							Application.m_VersionsByTime.f_Remove(Version);
						Version.m_VersionInfo = VersionInfo;
						Application.m_VersionsByTime.f_Insert(Version);

						pThis->fp_NewVersion(ApplicationName, Version);
						pFinishedPromise->f_SetResult();
					}
				;
			}
		;

		Auditor.f_Info(fg_Format("'{}' Starting upload of version", Desc));
		co_return fg_Move(StartUploadResult);;
	}
}
