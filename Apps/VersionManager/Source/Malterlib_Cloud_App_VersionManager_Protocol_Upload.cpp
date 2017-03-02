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
		{
			m_FileTransferReceive->f_Destroy();
			m_FileTransferReceive.f_Clear();
		}
	}

	CVersionManagerDaemonActor::CServer::CVersionUpload::CVersionUpload()
	{
	}

	auto CVersionManagerDaemonActor::CServer::fp_SaveVersionInfo(TCActor<> const &_FileActor, CStr const &_VersionPath, CVersionManager::CVersionInformation const &_VersionInfo) 
		-> TCContinuation<CSizeInfo> 
	{
		TCContinuation<CSizeInfo> Continuation;
		fg_Dispatch
			(
				_FileActor
				, [_VersionInfo, _VersionPath]() mutable -> CSizeInfo
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
					CFile::fs_WriteStringToFile(VersionInfoPath, VersionJSONInfo.f_ToString());
					
					return SizeInfo;
				}
			)
			> Continuation
		;
		return Continuation;
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_UploadVersion(CStartUploadVersion &&_Params) -> TCContinuation<CStartUploadVersion::CResult>
	{
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
			
		auto Auditor = pThis->mp_AppState.f_Auditor();
		CStr CallingHostID = fg_GetCallingHostID();
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
			return Auditor.f_Exception({"Invalid application format", "(start upload version)"});

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionIDAndPlatform.m_VersionID, ErrorStr))
				return Auditor.f_Exception({fg_Format("Invalid version ID format: {}", ErrorStr), "(start upload version)"});
		}
		
		if (!CVersionManager::fs_IsValidPlatform(_Params.m_VersionIDAndPlatform.m_Platform))
			return Auditor.f_Exception({"Invalid version platform format", "(start upload version)"});
		
		bool bFullAccess = pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "Application/WriteAll");
		
		while (!bFullAccess)
		{
			if (pThis->mp_Permissions.f_HostHasPermission(CallingHostID, fg_Format("Application/Write/{}", _Params.m_Application)))
				break;
			return Auditor.f_AccessDenied("(Start upload version)");
		}
		
		TCSet<CStr> DeniedTags;

		auto VersionInfo = _Params.m_VersionInfo;
		
		VersionInfo.m_Tags = pThis->fp_FilterTags(CallingHostID, VersionInfo.m_Tags, DeniedTags);

		CStr UploadID = fg_RandomID();
		
		auto &Upload = pThis->mp_VersionUploads[UploadID];
		Upload.m_Desc = fg_Format("{}", _Params.m_VersionIDAndPlatform);
		auto pCleanup = fg_OnScopeExitShared
			(
				[pThis, UploadID, ThisWeak = fg_ThisActor(pThis).f_Weak(), Desc = Upload.m_Desc, Auditor]
				{
					fg_Dispatch
						(
							ThisWeak
							, [pThis, UploadID, Desc, Auditor]
							{
								if (pThis->mp_VersionUploads.f_Remove(UploadID))
									Auditor.f_Error(fg_Format("'{}' Aborted upload of version", Desc));
							}
						)
						> fg_DiscardResult()
					;
				}
			)
		;
		
		CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Params.m_VersionIDAndPlatform.f_EncodeFileName());
		
		Upload.m_UploadFileAccess = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Upload version file access"));
		Upload.m_FileTransferReceive = fg_ConstructActor<CFileTransferReceive>(VersionPath, Upload.m_UploadFileAccess);
		
		TCContinuation<CVersionManager::CStartUploadVersion::CResult> Continuation;
		
		auto ReceiveFlags = CFileTransferReceive::EReceiveFlag_FailOnExisting;
		if (_Params.m_Flags & CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite)
			ReceiveFlags = CFileTransferReceive::EReceiveFlag_DeleteExisting; 
		
		Upload.m_FileTransferReceive(&CFileTransferReceive::f_ReceiveFiles, _Params.m_QueueSize, ReceiveFlags) 
			> 
			[
				pCleanup
				, pThis
				, UploadID
				, Continuation
				, Desc = Upload.m_Desc
				, _Params, VersionInfo
				, VersionPath
				, VersionID = _Params.m_VersionIDAndPlatform
				, ApplicationName = _Params.m_Application
				, DeniedTags
				, Auditor
			]
			(TCAsyncResult<CFileTransferContext> &&_FileTransferContext) mutable
			{
				if (!_FileTransferContext)
				{
					CStr Error = _FileTransferContext.f_GetExceptionStr();
					Auditor.f_Error(fg_Format("'{}' Failed to initialize version upload: {}", Desc, Error));
					if (Error == "Failed to extract current manifest: Directory already exists")
						Continuation.f_SetException(DMibErrorInstance("Version already exists"));
					else
						Continuation.f_SetException(DMibErrorInstance("Failed to receive files. See Version Manager log."));
					return;
				}
				auto *pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
				if (!pUpload)
					return;
				auto &Upload = *pUpload;
				
				CVersionManager::CStartUploadTransfer StartTransfer;
				StartTransfer.m_TransferContext = fg_Move(*_FileTransferContext);
				fg_Dispatch
					(
						_Params.m_DispatchActor
						, [fStartTransfer = fg_Move(_Params.m_fStartTransfer), StartTransfer = fg_Move(StartTransfer)]() mutable
						{
							return fStartTransfer(fg_Move(StartTransfer));
						}
					)
					> [pThis, Continuation, Desc, pCleanup, UploadID, DeniedTags, Auditor]
					(TCAsyncResult<CVersionManager::CStartUploadTransfer::CResult> &&_Result)
					{
						if (!_Result)
						{
							CStr Error = Auditor.f_Error({"Failed to start transfer. See Version Manager log.", fg_Format("'{}': {}", Desc, _Result.f_GetExceptionStr())});
							Continuation.f_SetException(DMibErrorInstance(Error));
							return;
						}

						auto *pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
						if (!pUpload)
							return;
						auto &Upload = *pUpload;
						
						Upload.m_DownloadSubscription = fg_Move(_Result->m_Subscription); 
						CVersionManager::CStartUploadVersion::CResult Result;
						Result.m_DeniedTags = DeniedTags;
						Result.m_Subscription = fg_ActorSubscription
							(
								fg_ThisActor(pThis)
								, [pThis, UploadID, Desc, Auditor]
								{
									auto *pUpload = pThis->mp_VersionUploads.f_FindEqual(UploadID);
									if (!pUpload)
										return;
									if (pUpload->m_FileTransferReceive)
									{
										pUpload->m_FileTransferReceive->f_Destroy();
										pUpload->m_FileTransferReceive.f_Clear();
									}
									if (pThis->mp_VersionUploads.f_Remove(UploadID))
										Auditor.f_Error(fg_Format("'{}' Aborted upload of version", Desc));
								}
							)
						;
						Continuation.f_SetResult(fg_Move(Result));
						pCleanup->f_Clear();
					}
				;
				
				Upload.m_FileTransferReceive(&CFileTransferReceive::f_GetResult) 
					> [pThis, UploadID, ApplicationName, VersionID, VersionInfo, VersionPath, Desc, Auditor](TCAsyncResult<CFileTransferResult> &&_Result)
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
							return;
						if (pUpload->m_FileTransferReceive)
						{
							pUpload->m_FileTransferReceive->f_Destroy();
							pUpload->m_FileTransferReceive.f_Clear();
						}
						auto FileAccess = pUpload->m_UploadFileAccess;
						pThis->mp_VersionUploads.f_Remove(UploadID);
						
						if (!_Result)
							return;
						
						pThis->fp_SaveVersionInfo(FileAccess, VersionPath, VersionInfo)
							> [pThis, VersionID, VersionInfo, Desc, ApplicationName, Auditor](TCAsyncResult<CSizeInfo> &&_InfoWriteResult) mutable
							{
								if (!_InfoWriteResult)
								{
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
							}
						;
					}
				;
			}
		;
		
		Auditor.f_Info(fg_Format("'{}' Starting upload of version", Upload.m_Desc));
		
		return Continuation;
	}
}
