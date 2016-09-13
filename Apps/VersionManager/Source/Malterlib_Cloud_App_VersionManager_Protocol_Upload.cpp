// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/ActorCallbackManager>

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

	auto CVersionManagerDaemonActor::CServer::fp_Protocol_UploadVersion(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CStartUploadVersion &&_Params)
		-> TCContinuation<CVersionManager::CStartUploadVersion::CResult> 
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
		
		bool bFullAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "Application/WriteAll");
		
		while (!bFullAccess)
		{
			if (mp_Permissions.f_HostHasPermission(_CallingHostInfo.f_GetRealHostID(), fg_Format("Application/Write/{}", _Params.m_Application)))
				break;
			
			return fp_AccessDenied(_CallingHostInfo, "Start upload version");
		}

		auto VersionInfo = _Params.m_VersionInfo;
		
		NStr::CStr UploadID = fg_RandomID();
		
		auto &Upload = mp_VersionUploads[UploadID];
		Upload.m_Desc = fg_Format("{}", _Params.m_VersionID);
		auto pCleanup = fg_OnScopeExitShared
			(
				[this, UploadID, ThisWeak = fg_ThisActor(this).f_Weak(), _CallingHostInfo, Desc = Upload.m_Desc]
				{
					fg_Dispatch
						(
							ThisWeak
							, [this, UploadID, _CallingHostInfo, Desc]
							{
								if (mp_VersionUploads.f_Remove(UploadID))
									fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Aborted upload of version", Desc));
							}
						)
						> fg_DiscardResult()
					;
				}
			)
		;
		
		CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Params.m_VersionID.f_EncodeFileName());
		
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
				, this
				, _CallingHostInfo
				, UploadID
				, Continuation
				, Desc = Upload.m_Desc
				, _Params, VersionInfo
				, VersionPath
				, VersionID = _Params.m_VersionID
				, ApplicationName = _Params.m_Application
			]
			(TCAsyncResult<CFileTransferContext> &&_FileTransferContext) mutable
			{
				if (!_FileTransferContext)
				{
					CStr Error = _FileTransferContext.f_GetExceptionStr();
					fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to initialize version upload: {}", Desc, Error));
					if (Error == "Failed to extract current manifest: Directory already exists")
						Continuation.f_SetException(DMibErrorInstance("Version already exists"));
					else
						Continuation.f_SetException(DMibErrorInstance("Failed to receive files. Consult version manager log files for more info."));
					return;
				}
				auto *pUpload = mp_VersionUploads.f_FindEqual(UploadID);
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
					> [this, Continuation, Desc, pCleanup, _CallingHostInfo, UploadID]
					(TCAsyncResult<CVersionManager::CStartUploadTransfer::CResult> &&_Result)
					{
						if (!_Result)
						{
							fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to start transfer for version upload on remote: {}", Desc, _Result.f_GetExceptionStr()));
							Continuation.f_SetException(DMibErrorInstance("Failed to start transfer. Consult version manager log files for more info."));
							return;
						}

						auto *pUpload = mp_VersionUploads.f_FindEqual(UploadID);
						if (!pUpload)
							return;
						auto &Upload = *pUpload;
						
						Upload.m_DownloadSubscription = fg_Move(_Result->m_Subscription); 
						CVersionManager::CStartUploadVersion::CResult Result;
						Result.m_Subscription = fg_ActorSubscription
							(
								fg_ThisActor(this)
								, [this, UploadID, Desc, _CallingHostInfo]
								{
									auto *pUpload = mp_VersionUploads.f_FindEqual(UploadID);
									if (!pUpload)
										return;
									if (pUpload->m_FileTransferReceive)
									{
										pUpload->m_FileTransferReceive->f_Destroy();
										pUpload->m_FileTransferReceive.f_Clear();
									}
									if (mp_VersionUploads.f_Remove(UploadID))
									{
										fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Aborted upload of version", Desc));
									}
								}
							)
						;
						Continuation.f_SetResult(fg_Move(Result));
						pCleanup->f_Clear();
					}
				;
				
				Upload.m_FileTransferReceive(&CFileTransferReceive::f_GetResult) 
					> [this, _CallingHostInfo, UploadID, ApplicationName, VersionID, VersionInfo, VersionPath, Desc](TCAsyncResult<CFileTransferResult> &&_Result)
					{
						if (!_Result)
							fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to transfer version (upload): {}", Desc, _Result.f_GetExceptionStr()));
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
										"'{}' Version upload finished transferring: {}"
										, Desc
										, Message
									)
								)
							;
						}
						
						auto *pUpload = mp_VersionUploads.f_FindEqual(UploadID);
						if (!pUpload)
							return;
						if (pUpload->m_FileTransferReceive)
						{
							pUpload->m_FileTransferReceive->f_Destroy();
							pUpload->m_FileTransferReceive.f_Clear();
						}
						auto FileAccsess = pUpload->m_UploadFileAccess;
						mp_VersionUploads.f_Remove(UploadID);
						
						if (!_Result)
							return;
						
						fg_Dispatch
							(
								FileAccsess
								, [VersionInfo, VersionPath]() mutable
								{
									CEJSON VersionJSONInfo;
									CStr VersionInfoPath = fg_Format("{}.json", VersionPath);
									
									VersionJSONInfo["Time"] = VersionInfo.m_Time;
									VersionJSONInfo["Configuration"] = VersionInfo.m_Configuration;
									VersionJSONInfo["ExtraInfo"] = VersionInfo.m_ExtraInfo;
									
									CFile::fs_CreateDirectory(CFile::fs_GetPath(VersionInfoPath));
									CFile::fs_WriteStringToFile(VersionInfoPath, VersionJSONInfo.f_ToString());
								}
							)
							> [this, VersionID, VersionInfo, _CallingHostInfo, Desc, ApplicationName](TCAsyncResult<void> &&_InfoWriteResult)
							{
								if (!_InfoWriteResult)
								{
									fsp_LogActivityError(_CallingHostInfo, fg_Format("'{}' Failed to write version info file: {}", Desc, _InfoWriteResult.f_GetExceptionStr()));
									return;
								}
								
								auto ApplicationMapped = mp_Applications(ApplicationName); 
								auto &Application = *ApplicationMapped;
								
								if (ApplicationMapped.f_WasCreated())
								{
									TCSet<CStr> Permissions;
									{
										Permissions[fg_Format("Application/Read/{}", ApplicationName)];
										Permissions[fg_Format("Application/Write/{}", ApplicationName)];
									}
									mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
								}
								
								auto &Version = Application.m_Versions[VersionID];
								if (Version.m_TimeLink.f_IsInTree())
									Application.m_VersionsByTime.f_Remove(Version);
								Version.m_VersionInfo = VersionInfo;
								Application.m_VersionsByTime.f_Insert(Version);
								CVersionManager::CNewVersionNotification NewVersionNotification;
								NewVersionNotification.m_Application = ApplicationName;
								NewVersionNotification.m_VersionID = VersionID;
								NewVersionNotification.m_VersionInfo = VersionInfo;
								
								TCVector<CStr> Permissions;
								Permissions.f_Insert("Application/ReadAll");
								Permissions.f_Insert("Application/ListAll");
								Permissions.f_Insert(fg_Format("Application/Read/{}", ApplicationName));
								
								auto fSendToSubscription = [&](CSubscription const &_Subscription)
									{
										if (!mp_Permissions.f_HostHasAnyPermission(_Subscription.m_HostID, Permissions))
											return;
										_Subscription.f_SendVersion(NewVersionNotification);
									}
								;
								for (auto &Subscription : mp_GlobalVersionSubscriptions)
									fSendToSubscription(Subscription);
								auto *pSubscription = mp_VersionSubscriptions.f_FindEqual(ApplicationName);
								if (pSubscription)
								{
									for (auto &Subscription : *pSubscription)
										fSendToSubscription(Subscription);
								}
							}
						;
					}
				;
			}
		;
		
		fsp_LogActivityInfo(_CallingHostInfo, fg_Format("'{}' Starting upload of version", Upload.m_Desc));
		
		return Continuation;
	}
}
