// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Cryptography/EncryptedStream>
#include <Mib/Cloud/SecretsManagerUpload>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	using CEncryptedStream = TCBinaryStream_Encrypted<NPtr::TCUniquePointer<NStream::CBinaryStream>>;

	CSecretsManagerDaemonActor::CServer::CDownload::CDownload()
	{
	}

	CSecretsManagerDaemonActor::CServer::CDownload::~CDownload()
	{
		if (m_DirectorySyncSend)
		{
			m_DirectorySyncSend->f_DestroyNoResult(DMibPFile, DMibPLine);
			m_DirectorySyncSend.f_Clear();
		}
	}

	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CDownload::f_Destroy()
	{
		auto DirectorySend = fg_Move(m_DirectorySyncSend);
		TCContinuation<void> SubscriptionContinuation;
		if (m_Subscription)
			m_Subscription->f_Destroy().f_Dispatch().f_Timeout(10.0, "Timed out waiting for secret download to destroy") > SubscriptionContinuation;
		else
			SubscriptionContinuation.f_SetResult();

		TCContinuation<void> FileSubscriptionContinuation;
		if (m_FileSubscription)
			m_Subscription->f_Destroy().f_Dispatch().f_Timeout(10.0, "Timed out waiting for secret download to destroy") > FileSubscriptionContinuation;
		else
			FileSubscriptionContinuation.f_SetResult();

		TCContinuation<void> Continuation;
		SubscriptionContinuation + FileSubscriptionContinuation > [=](auto &&, auto &&)
			{
				if (DirectorySend)
					DirectorySend->f_Destroy() > Continuation;
				else
					Continuation.f_SetResult();
			}
		;

		return Continuation;
	}

	CSecretsManagerDaemonActor::CServer::CUpload::CUpload()
	{
	}

	CSecretsManagerDaemonActor::CServer::CUpload::~CUpload()
	{
		if (m_DirectorySyncReceive)
		{
			m_DirectorySyncReceive->f_DestroyNoResult(DMibPFile, DMibPLine);
			m_DirectorySyncReceive.f_Clear();
		}
	}

	TCContinuation<void> CSecretsManagerDaemonActor::CServer::CUpload::f_Destroy()
	{
		auto DirectoryReceive = fg_Move(m_DirectorySyncReceive);
		TCContinuation<void> Continuation;
		if (DirectoryReceive)
			DirectoryReceive->f_Destroy() > Continuation;
		else
			Continuation.f_SetResult();

		return Continuation;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_DownloadFile(CSecretID &&_ID, NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
		-> TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
	{
		TCContinuation<TCDistributedActorInterfaceWithID<CDirectorySyncClient>> Continuation;
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/DownloadFile"))
			return Auditor.f_AccessDenied("(DownloadFile, command)");

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if	(!This.fp_HasPermission("Read", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(DownloadFile, no permission for '{}')", Permission));

			if (pSecretProperty->m_Secret.f_GetTypeID() != CSecretsManager::ESecretType_File)
				return Auditor.f_Exception(fg_Format("Secret '{}' does not contain a file secret", _ID));

			CDirectoryManifest Manifest;
			auto &ManifestFile = pSecretProperty->m_Secret.f_Get<CSecretsManager::ESecretType_File>().m_Manifest;
			Manifest.m_Files[ManifestFile.m_OriginalPath] = ManifestFile;

			CDirectorySyncSend::CConfig Config;
			Config.m_BasePath = This.mp_AppState.m_RootDirectory + "/SecretsManagerFiles";
			Config.m_Manifest = fg_Move(Manifest);
			Config.m_FileOptions.m_fOpenStream = [Key = pSecretProperty->m_Key, IV = pSecretProperty->m_IV, HMACKey = pSecretProperty->m_HMACKey]
				(
				 	CStr const &_FileName
				 	, EDirectorySyncStreamType _FileType
				 	, EFileOpen _OpenFlags
				 	, EFileAttrib _Attributes
				)
				-> NPtr::TCUniquePointer<NStream::CBinaryStream>
				{
					DRequire(_FileType == EDirectorySyncStreamType_Source);

					TCUniquePointer<TCBinaryStreamFile<>> pFile = fg_Construct();
					pFile->f_Open(_FileName, _OpenFlags, _Attributes);

					NPtr::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, IV}, ESSLDigest_SHA512, HMACKey);
					pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Read);
					return pStream;
				}
			;

			Config.m_FileOptions.m_fTransformFilePath = [RandomFileName = pSecretProperty->m_RandomFileName, DownloadFile = ManifestFile.m_OriginalPath]
				(
				 	CStr const &_BasePath
				 	, CStr const &_FileName
				 	, EDirectorySyncStreamType _Type
				)
				-> CStr
				{
					if (_FileName != DownloadFile)
						DMibError("Unexpected file name in file manifest");

					if (_Type != EDirectorySyncStreamType_Source)
						DMibError("Unsupported sync file type");

					return CFile::fs_AppendPath(_BasePath, RandomFileName);
				}
			;
			auto DownloadID = fg_RandomID();
			auto &Download = This.mp_Downloads[DownloadID];
			Download.m_DirectorySyncSend = This.mp_AppState.m_DistributionManager->f_ConstructActor<CDirectorySyncSend>(fg_Move(Config));
			Download.m_Subscription = fg_Move(_Subscription);
			Download.m_FileSubscription = This.fp_ReserveFile(pSecretProperty->m_RandomFileName);

#if DMibConfig_Tests_Enable
			if (auto *pContinuation = This.mp_DownloadInitialized.f_FindEqual(_ID.m_Name))
			{
				pContinuation->f_SetResult();	// Tell the test that the download has reserved the file
				This.mp_DownloadInitialized.f_Remove(pContinuation);
			}
#endif

			TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncInterface
				{
					Download.m_DirectorySyncSend->f_ShareInterface<CDirectorySyncClient>()
					, g_ActorSubscription > [=, pThis = m_pThis, DownloadFile = ManifestFile.m_OriginalPath]() -> TCContinuation<void>
					{
						auto *pDownload = pThis->mp_Downloads.f_FindEqual(DownloadID);
						if (!pDownload)
							return fg_Explicit();

						auto &Download = *pDownload;

						if (!Download.m_DirectorySyncSend)
						{
							pThis->mp_Downloads.f_Remove(DownloadID);
							return Auditor.f_Exception("Sync aborted");
						}

						TCContinuation<void> Continuation;
						Download.m_DirectorySyncSend(&CDirectorySyncSend::f_GetResult) > [=](TCAsyncResult<CDirectorySyncSend::CSyncResult> &&_Result)
							{
								pThis->mp_Downloads.f_Remove(DownloadID);
								if (!_Result)
									Continuation.f_SetException(Auditor.f_Exception({"Error getting result for directory sync send", _Result.f_GetExceptionStr()}));
								else
								{
									auto &Result = *_Result;
									Continuation.f_SetResult();
									if (Result.m_Stats.m_nSyncedFiles <= 1)
										Auditor.f_Info("Download of '{}' from '{}' finished without transferring any content"_f << DownloadFile << _ID);
									else
									{
										Auditor.f_Info
											(
												"Download of '{}' from '{}' finished transferring: {ns } incoming bytes at {fe2} MB/s    {ns } outgoing bytes at {fe2} MB/s"_f
											 	<< DownloadFile
											 	<< _ID
												<< Result.m_Stats.m_IncomingBytes
												<< Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
												<< Result.m_Stats.m_OutgoingBytes
												<< Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
											)
										;
									}
								}
#if DMibConfig_Tests_Enable
								if (auto *pContinuation = pThis->mp_DownloadCompleted.f_FindEqual(_ID.m_Name))
								{
									pThis->f_SyncFileOperations() > *pContinuation / [Continuation = *pContinuation]
										{
											Continuation.f_SetResult();	// Tell the test the transfer has completed and any file ops should have completed
										}
									;
									pThis->mp_DownloadCompleted.f_Remove(pContinuation);
								}
#endif
							}
						;
						return Continuation;
					}
				}
			;
			Continuation.f_SetResult(fg_Move(SyncInterface));
		}
		else
			return Auditor.f_Exception(fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

		return Continuation;
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_UploadFile
		(
		 	CSecretID &&_ID
		 	, NStr::CStrSecure const &_FileName
		 	, TCDistributedActorInterfaceWithID<CDirectorySyncClient> &&_Uploader
		 )
		-> TCContinuation<NConcurrency::TCActorFunctorWithID<TCContinuation<void> ()>>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();
		auto CallingHostID = Auditor.f_HostInfo().f_GetRealHostID();

		if (!This.mp_Permissions.f_HostHasAnyPermission(CallingHostID, "SecretsManager/CommandAll", "SecretsManager/Command/UploadFile"))
			return Auditor.f_AccessDenied("(UploadFile, command)");

		if (_FileName.f_FindChars("*^?[]") != -1)
			return Auditor.f_Exception("The file name cannot contain any of the characters: '^*?[]'");

		TCContinuation<void> CheckResultContinuation;

		if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
		{
			CStr Permission;
			if	(!This.fp_HasPermission("Write", pSecretProperty->m_SemanticID, pSecretProperty->m_Tags, Permission))
				return Auditor.f_AccessDenied(fg_Format("(UploadFile, no permission for '{}')", Permission));

			CDirectorySyncReceive::CConfig Config;
			Config.m_PreviousBasePath = Config.m_BasePath = m_pThis->mp_AppState.m_RootDirectory + "/SecretsManagerFiles";
			Config.m_SyncFlags = CDirectorySyncReceive::ESyncFlag_None;

			if (pSecretProperty->m_Key.f_IsEmpty())
				pSecretProperty->m_Key = CEncryptKeyIV::fs_GetRandomKey(ESSLCrypto_AES_256_CBC);
			auto IV = CEncryptKeyIV::fs_GetRandomIV(ESSLCrypto_AES_256_CBC);
			auto HMACKey = CEncryptKeyIV::fs_GetRandomHMACKey(ESSLDigest_SHA512);
			auto NewFileName = fg_RandomID();

			TCSharedPointer<bool> pNewFileClaimed = fg_Construct(false);

			TCSharedPointer<CActorSubscription> pCleanupFile = fg_Construct
				(
					g_ActorSubscription > [=, pThis = m_pThis, pCanDestroy = This.mp_pCanDestroyFileActorTracker]
					{
						if (!*pNewFileClaimed)
							pThis->fp_RemoveFile(NewFileName, Auditor) > fg_DiscardResult();
					}
				)
			;

			Config.m_FileOptions.m_fOpenStream = [IV, Key = pSecretProperty->m_Key, OldIV = pSecretProperty->m_IV, OldHMACKey = pSecretProperty->m_HMACKey, HMACKey]
				(
				 	CStr const &_FileName
				 	, EDirectorySyncStreamType _FileType
				 	, EFileOpen _OpenFlags
				 	, EFileAttrib _Attributes
				)
				-> NPtr::TCUniquePointer<NStream::CBinaryStream>
				{
					if (_FileType & EDirectorySyncStreamType_Manifest)
					{
						// The secrets manager is only transfering a single file, the manifest file will be small, so we use a memory stream
						TCUniquePointer<CBinaryStreamMemory<>> pStream = fg_Construct();
						return pStream;
					}

					TCUniquePointer<TCBinaryStreamFile<>> pFile = fg_Construct();
					pFile->f_Open(_FileName, _OpenFlags, _Attributes);

					DCheck(_FileType == EDirectorySyncStreamType_Source || _FileType == EDirectorySyncStreamType_Destination);

					if (_FileType == EDirectorySyncStreamType_Source)
					{
						NPtr::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, OldIV}, ESSLDigest_SHA512, OldHMACKey);
						pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Read);
						return pStream;
					}
					else
					{
						NPtr::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, IV}, ESSLDigest_SHA512, HMACKey);
						pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Write);
						return pStream;
					}
				}
			;
			Config.m_FileOptions.m_fTransformFilePath = [UploadFileName = _FileName, OldFileName = pSecretProperty->m_RandomFileName, NewFileName]
				(
				 	CStr const &_BasePath
				 	, CStr const &_FileName
				 	, EDirectorySyncStreamType _Type
				)
				-> CStr
				{
					if (_FileName == UploadFileName)
					{
						if (_Type == EDirectorySyncStreamType_Source)
						{
							if (OldFileName)
								return CFile::fs_AppendPath(_BasePath, OldFileName);
							else
								return CFile::fs_AppendPath(_BasePath, fg_RandomID());
						}
						if (_Type == EDirectorySyncStreamType_Destination)
							return CFile::fs_AppendPath(_BasePath, NewFileName);
					}

					if (_Type & EDirectorySyncStreamType_Manifest)
						// The "<Internal>" prefix is a marker for the in memory streams for manifest files.
						return CFile::fs_AppendPath(CStr{"<Internal>"}, _FileName);

					return CFile::fs_AppendPath(_BasePath, _FileName);
				}
			;

			auto &Upload = This.mp_Uploads[NewFileName];
			Upload.m_DirectorySyncReceive = fg_ConstructActor<NFile::CDirectorySyncReceive>(fg_Move(Config), fg_Move(_Uploader));

			Upload.m_DirectorySyncReceive(&CDirectorySyncReceive::f_PerformSync)
				>
				[=, OldFileName = _FileName, SavedSecret = pSecretProperty->m_Secret]
				(TCAsyncResult<CDirectorySyncReceive::CSyncResult> &&_Result) mutable
				{
					auto &This = *m_pThis;

#if DMibConfig_Tests_Enable
					auto CleanupTest = g_OnScopeExit > [&]
						{
							if (auto *pContinuation = This.mp_UploadCompleted.f_FindEqual(OldFileName))
							{
								pContinuation->f_SetResult();	// Tell the test the transfer has completed
								This.mp_UploadCompleted.f_Remove(pContinuation);
							}
						}
					;
#endif
					auto Cleanup = NConcurrency::g_ActorSubscription > [this, NewFileName]
						{
							auto &This = *m_pThis;
							This.mp_Uploads.f_Remove(NewFileName);
						}
					;

					if (!_Result)
					{
						CheckResultContinuation.f_SetException
							(
							 	Auditor.f_Exception({"Internal error. Check SecretsManager.Log for details.", fg_Format("UploadFile - sync failed: {}", _Result.f_GetExceptionStr())})
							)
						;
						return;
					}

					auto &Result = *_Result;

					if (auto *pSecretProperty = This.mp_Database.m_Secrets.f_FindEqual(_ID))
					{
						if (pSecretProperty->m_Secret != SavedSecret)
						{

							(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
								{
									// For the problem with two competing simultaneous uploads we chose to let the one that completes first win.
									// Another alternative would have been to lock the secret during an upload and disallow other operations during the upload,
									// but with that solution we would have to handle problems with disconnects and transfers timing out to avoid ending up with
									// locked secrets. This way we also handle the case when someone changes the secret to a string or binary secret during upload.
									// We can just remove the uploaded file and report an error.
									CStr Error = "The secret property in secret '{}' was changed while the secret file was uploaded. Please check and upload again."_f << _ID;
									CheckResultContinuation.f_SetException(Auditor.f_Exception(Error));
								}
							;
							return;
						}

						if (Result.m_Manifest.m_Files.f_GetLen() == 1)
						{
							*pNewFileClaimed = true;
							This.fp_RemoveUnreferencedFile(pSecretProperty->m_RandomFileName, Auditor) > fg_DiscardResult();
							auto ManifestFile = *Result.m_Manifest.m_Files.f_FindAny();
							pSecretProperty->m_Secret = CSecretsManager::CSecret{CSecretsManager::CSecretFile{ManifestFile}};
							pSecretProperty->m_Modified = CTime::fs_NowUTC();
							pSecretProperty->m_IV = fg_Move(IV);
							pSecretProperty->m_HMACKey = fg_Move(HMACKey);
							pSecretProperty->m_RandomFileName = fg_Move(NewFileName);

							This.fp_WriteDatabase();

							if (Result.m_Stats.m_nSyncedFiles <= 1)
								Auditor.f_Info("Upload of '{}' for '{}' finished without conent changes"_f << ManifestFile.m_OriginalPath << _ID);
							else
							{
								Auditor.f_Info
									(
										"Upload of '{}' for '{}' finished transferring: {ns } incoming bytes at {fe2} MB/s    {ns } outgoing bytes at {fe2} MB/s"_f
									 	<< ManifestFile.m_OriginalPath
									  	<< _ID
										<< Result.m_Stats.m_IncomingBytes
										<< Result.m_Stats.f_IncomingBytesPerSecond()/1'000'000.0
										<< Result.m_Stats.m_OutgoingBytes
										<< Result.m_Stats.f_OutgoingBytesPerSecond()/1'000'000.0
									)
								;
							}

						}
						else
						{
							(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
								{
									CStr Error = "{} files in the manifest? This was unexpected"_f << Result.m_Manifest.m_Files.f_GetLen();
									CheckResultContinuation.f_SetException(Auditor.f_Exception(Error));
								}
							;
						}
					}
					else
					{
						// The secret was removed while the file was transferred. Ooops.
						(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
							{
								CheckResultContinuation.f_SetException(Auditor.f_Exception("Secret '{}' removed while the secret file was uploaded"_f << _ID));
							}
						;
					}
				}
			;

#if DMibConfig_Tests_Enable
			if (auto *pContinuation = This.mp_UploadInitialized.f_FindEqual(_FileName))
			{
				pContinuation->f_SetResult();	// Tell the test the transfer has been initialized (SaveSecret has been set so we know if it has changed when the transfer completes)
				This.mp_UploadInitialized.f_Remove(pContinuation);
			}
#endif
			return fg_Explicit
				(
				 	g_ActorFunctor
				 	(
						g_ActorSubscription > [this, NewFileName, AllowDestroy = g_AllowWrongThreadDestroy]
						{
							auto &This = *m_pThis;
							This.mp_Uploads.f_Remove(NewFileName);
						}
					)
				 	> [CheckResultContinuation, AllowDestroy = g_AllowWrongThreadDestroy]() -> TCContinuation<void>
				 	{
						if (!CheckResultContinuation.f_IsSet())
							CheckResultContinuation.f_SetResult();

						return CheckResultContinuation;
					}
				)
			;
		}
		else
			return Auditor.f_Exception(fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));
	}
 }
