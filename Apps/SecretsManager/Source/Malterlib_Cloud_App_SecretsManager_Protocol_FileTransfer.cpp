// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/EncryptedStream>
#include <Mib/Cloud/SecretsManagerUpload>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	using CEncryptedStream = TCBinaryStream_Encrypted<NStorage::TCUniquePointer<NStream::CBinaryStream>>;

	CSecretsManagerDaemonActor::CServer::CDownload::CDownload() = default;
	CSecretsManagerDaemonActor::CServer::CDownload::CDownload(CDownload &&) = default;

	CSecretsManagerDaemonActor::CServer::CDownload::~CDownload()
	{
		if (m_DirectorySyncSend)
			fg_Move(m_DirectorySyncSend).f_Destroy() > fg_LogWarning("Mib/Cloud/SecretsManager", "Failed to destroy directory sync send in destructor");
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CDownload::f_Destroy()
	{
		auto This = co_await fg_MoveThis(*this);

		CLogError LogError("Mib/Cloud/SecretsManager");

		TCFutureVector<void> Destroys;

		auto DirectorySend = fg_Move(This.m_DirectorySyncSend);
		if (This.m_Subscription)
			This.m_Subscription->f_Destroy().f_Timeout(10.0, "Timed out waiting for secret download to destroy") > Destroys;

		if (This.m_FileSubscription)
			This.m_Subscription->f_Destroy().f_Timeout(10.0, "Timed out waiting for secret download to destroy") > Destroys;

		co_await fg_AllDone(Destroys).f_Wrap() > LogError.f_Warning("Failed to destroy download");

		if (DirectorySend)
			co_await fg_Move(DirectorySend).f_Destroy().f_Wrap() > LogError.f_Warning("Failed to destroy directory sync send in destroy");

		co_return {};
	}

	CSecretsManagerDaemonActor::CServer::CUpload::CUpload() = default;
	CSecretsManagerDaemonActor::CServer::CUpload::CUpload(CUpload &&) = default;

	CSecretsManagerDaemonActor::CServer::CUpload::~CUpload()
	{
		if (m_DirectorySyncReceive)
			fg_Move(m_DirectorySyncReceive).f_Destroy().f_DiscardResult();
	}

	TCFuture<void> CSecretsManagerDaemonActor::CServer::CUpload::f_Destroy()
	{
		auto This = co_await fg_MoveThis(*this);

		if (This.m_DirectorySyncReceive)
			co_await fg_Move(This.m_DirectorySyncReceive).f_Destroy();
		co_return {};
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_DownloadFile(CSecretID _ID, NConcurrency::TCActorSubscriptionWithID<> _Subscription)
		-> TCFuture<TCDistributedActorInterfaceWithID<CDirectorySyncClient>>
	{
		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/DownloadFile"}};
		auto *pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Read", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Download file from SecretsManager", Permissions) % "Permission denied downloading file" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(DownloadFile, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(DownloadFile, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID);
		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

		CStr Permission;
		if (pSecretProperties->m_Secret.f_GetTypeID() != CSecretsManager::ESecretType_File)
			co_return Auditor.f_Exception(fg_Format("Secret '{}' does not contain a file secret", _ID));

		CDirectoryManifest Manifest;
		auto &ManifestFile = pSecretProperties->m_Secret.f_Get<CSecretsManager::ESecretType_File>().m_Manifest;
		Manifest.m_Files[ManifestFile.m_OriginalPath] = ManifestFile;

		CDirectorySyncSend::CConfig Config;
		Config.m_BasePath = This.mp_AppState.m_RootDirectory + "/SecretsManagerFiles";
		Config.m_Manifest = fg_Move(Manifest);
		Config.m_FileOptions.m_fOpenStream = [Key = pSecretProperties->m_Key, IV = pSecretProperties->m_IV, HMACKey = pSecretProperties->m_HMACKey]
			(
				CStr const &_FileName
				, EDirectorySyncStreamType _FileType
				, EFileOpen _OpenFlags
				, EFileAttrib _Attributes
			)
			-> NStorage::TCUniquePointer<NStream::CBinaryStream>
			{
				DRequire(_FileType == EDirectorySyncStreamType_Source);

				TCUniquePointer<TCBinaryStreamFile<>> pFile = fg_Construct();
				pFile->f_Open(_FileName, _OpenFlags, _Attributes);

				NStorage::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, IV}, EDigestType_SHA512, HMACKey);
				pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Read);
				return pStream;
			}
		;

		Config.m_FileOptions.m_fTransformFilePath = [RandomFileName = pSecretProperties->m_RandomFileName, DownloadFile = ManifestFile.m_OriginalPath]
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
		auto DownloadID = fg_RandomID(This.mp_Downloads);
		auto &Download = This.mp_Downloads[DownloadID];
		Download.m_DirectorySyncSend = This.mp_AppState.m_DistributionManager->f_ConstructActor<CDirectorySyncSend>(fg_Move(Config));
		Download.m_Subscription = fg_Move(_Subscription);
		Download.m_FileSubscription = This.fp_ReserveFile(pSecretProperties->m_RandomFileName);

#if DMibConfig_Tests_Enable
		if (auto *pPromise = This.mp_DownloadInitialized.f_FindEqual(_ID.m_Name))
		{
			pPromise->f_SetResult();	// Tell the test that the download has reserved the file
			This.mp_DownloadInitialized.f_Remove(pPromise);
		}
#endif

		TCDistributedActorInterfaceWithID<CDirectorySyncClient> SyncInterface
			{
				Download.m_DirectorySyncSend->f_ShareInterface<CDirectorySyncClient>()
				, g_ActorSubscription / [=, pThis = m_pThis, DownloadFile = ManifestFile.m_OriginalPath]() -> TCFuture<void>
				{
					auto *pDownload = pThis->mp_Downloads.f_FindEqual(DownloadID);
					if (!pDownload)
						co_return {};

					auto &Download = *pDownload;

					if (!Download.m_DirectorySyncSend)
					{
						pThis->mp_Downloads.f_Remove(DownloadID);
						Auditor.f_Exception("Sync aborted");
						co_return {};
					}

					auto GetResultResults = co_await Download.m_DirectorySyncSend(&CDirectorySyncSend::f_GetResult).f_Wrap();
#if DMibConfig_Tests_Enable
					if (auto *pPromise = pThis->mp_DownloadCompleted.f_FindEqual(_ID.m_Name))
					{
						pThis->f_SyncFileOperations() > *pPromise / [Promise = *pPromise]
							{
								Promise.f_SetResult();	// Tell the test the transfer has completed and any file ops should have completed
							}
						;
						pThis->mp_DownloadCompleted.f_Remove(pPromise);
					}
#endif
					pThis->mp_Downloads.f_Remove(DownloadID);
					if (!GetResultResults)
						co_return Auditor.f_Exception({"Error getting result for directory sync send", GetResultResults.f_GetExceptionStr()});

					auto &Result = *GetResultResults;

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

					co_return {};
				}
			}
		;
		co_return fg_Move(SyncInterface);
	}

	auto CSecretsManagerDaemonActor::CServer::CSecretsManagerImplementation::f_UploadFile
		(
			CSecretID _ID
			, NStr::CStrSecure _FileName
			, TCDistributedActorInterfaceWithID<CDirectorySyncClient> _Uploader
		 )
		-> TCFuture<NConcurrency::TCActorFunctorWithID<TCFuture<void> ()>>
	{
		if (!_Uploader)
			co_return DMibErrorInstance("Invalid uploader");

		auto &This = *m_pThis;
		auto Auditor = This.mp_AppState.f_Auditor();

		if (_FileName.f_FindChars("*^?[]") != -1)
			co_return Auditor.f_Exception("The file name cannot contain any of the characters: '^*?[]'");

		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

		Permissions["Command"] = {{"SecretsManager/CommandAll", "SecretsManager/Command/UploadFile"}};
		auto *pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID);
		if (pSecretProperties)
			fsp_AddPermissionQueryIndexedByPermission("Write", pSecretProperties->m_SemanticID, pSecretProperties->m_Tags, Permissions);

		auto HasPermissions = co_await (This.mp_Permissions.f_HasPermissions("Upload file to SecretsManager", Permissions) % "Permission denied uploading file" % Auditor);

		if (!HasPermissions["Command"])
			co_return Auditor.f_AccessDenied("(UploadFile, command)", Permissions["Command"]);

		for (auto const &bHasPermission : HasPermissions)
		{
			if (!bHasPermission)
			{
				auto &PermissionKey = HasPermissions.fs_GetKey(bHasPermission);
				co_return Auditor.f_AccessDenied(fg_Format("(UploadFile, no permission for '{}')", PermissionKey), Permissions[PermissionKey]);
			}
		}

		pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID);
		if (!pSecretProperties)
			co_return Auditor.f_Exception(fg_Format("No secret matching ID: '{}/{}'", _ID.m_Folder, _ID.m_Name));

		CDirectorySyncReceive::CConfig Config;
		Config.m_PreviousBasePath = Config.m_BasePath = m_pThis->mp_AppState.m_RootDirectory + "/SecretsManagerFiles";
		Config.m_SyncFlags = CDirectorySyncReceive::ESyncFlag_None;

		if (pSecretProperties->m_Key.f_IsEmpty())
			pSecretProperties->m_Key = CEncryptKeyIV::fs_GetRandomKey(ECryptoType_AES_256_CBC);
		auto IV = CEncryptKeyIV::fs_GetRandomIV(ECryptoType_AES_256_CBC);
		auto HMACKey = CEncryptKeyIV::fs_GetRandomHMACKey(EDigestType_SHA512);
		auto NewFileName = fg_RandomID();

		TCSharedPointer<bool> pNewFileClaimed = fg_Construct(false);

		TCSharedPointer<CActorSubscription> pCleanupFile = fg_Construct
			(
				g_ActorSubscription / [=, pThis = m_pThis, pCanDestroy = This.mp_pCanDestroyFileActorTracker]() -> TCFuture<void>
				{
					if (!*pNewFileClaimed)
						co_await pThis->fp_RemoveFile(NewFileName, Auditor);

					co_return {};
				}
			)
		;

		Config.m_FileOptions.m_fOpenStream = [IV, Key = pSecretProperties->m_Key, OldIV = pSecretProperties->m_IV, OldHMACKey = pSecretProperties->m_HMACKey, HMACKey]
			(
				CStr const &_FileName
				, EDirectorySyncStreamType _FileType
				, EFileOpen _OpenFlags
				, EFileAttrib _Attributes
			)
			-> NStorage::TCUniquePointer<NStream::CBinaryStream>
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
					NStorage::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, OldIV}, EDigestType_SHA512, OldHMACKey);
					pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Read);
					return pStream;
				}
				else
				{
					NStorage::TCUniquePointer<CEncryptedStream> pStream = fg_Construct<CEncryptedStream>(CEncryptKeyIV{Key, IV}, EDigestType_SHA512, HMACKey);
					pStream->f_Open(fg_Move(pFile), NFile::EFileOpen_Write);
					return pStream;
				}
			}
		;
		Config.m_FileOptions.m_fTransformFilePath = [UploadFileName = _FileName, OldFileName = pSecretProperties->m_RandomFileName, NewFileName]
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

		TCPromiseFuturePair<void> CheckResultPromise;

		Upload.m_DirectorySyncReceive(&CDirectorySyncReceive::f_PerformSync) >
			[=, this, OldFileName = _FileName, SavedSecret = pSecretProperties->m_Secret, CheckResultPromise = fg_Move(CheckResultPromise.m_Promise)]
			(TCAsyncResult<CDirectorySyncReceive::CSyncResult> &&_Result) mutable
			{
				auto &This = *m_pThis;

#if DMibConfig_Tests_Enable
				auto CleanupTest = g_OnScopeExit / [&]
					{
						if (auto *pPromise = This.mp_UploadCompleted.f_FindEqual(OldFileName))
						{
							pPromise->f_SetResult();	// Tell the test the transfer has completed
							This.mp_UploadCompleted.f_Remove(pPromise);
						}
					}
				;
#endif
				auto Cleanup = NConcurrency::g_ActorSubscription / [this, NewFileName]
					{
						auto &This = *m_pThis;
						This.mp_Uploads.f_Remove(NewFileName);
					}
				;

				if (!_Result)
				{
					CheckResultPromise.f_SetException
						(
							Auditor.f_Exception({"Internal error. Check SecretsManager.Log for details.", fg_Format("UploadFile - sync failed: {}", _Result.f_GetExceptionStr())})
						)
					;
					return;
				}

				auto &Result = *_Result;

				if (auto *pSecretProperties = This.mp_Database.m_Secrets.f_FindEqual(_ID))
				{
					if (pSecretProperties->m_Secret != SavedSecret)
					{
						(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
							{
								// For the problem with two competing simultaneous uploads we chose to let the one that completes first win.
								// Another alternative would have been to lock the secret during an upload and disallow other operations during the upload,
								// but with that solution we would have to handle problems with disconnects and transfers timing out to avoid ending up with
								// locked secrets. This way we also handle the case when someone changes the secret to a string or binary secret during upload.
								// We can just remove the uploaded file and report an error.
								CStr Error = "The secret property in secret '{}' was changed while the secret file was uploaded. Please check and upload again."_f << _ID;
								CheckResultPromise.f_SetException(Auditor.f_Exception(Error));
							}
						;
						return;
					}

					if (Result.m_Manifest.m_Files.f_GetLen() == 1)
					{
						*pNewFileClaimed = true;
						This.fp_RemoveUnreferencedFile(pSecretProperties->m_RandomFileName, Auditor).f_DiscardResult();
						auto ManifestFile = *Result.m_Manifest.m_Files.f_FindAny();
						pSecretProperties->m_Secret = CSecretsManager::CSecret{CSecretsManager::CSecretFile{ManifestFile}};
						pSecretProperties->m_Modified = CTime::fs_NowUTC();
						pSecretProperties->m_IV = fg_Move(IV);
						pSecretProperties->m_HMACKey = fg_Move(HMACKey);
						pSecretProperties->m_RandomFileName = fg_Move(NewFileName);

						This.fp_SecretUpdated(*pSecretProperties, false);

						This.fp_WriteDatabase() > CheckResultPromise % "Falied to write database" / [=]
							{
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
								CheckResultPromise.f_SetResult();
							}
						;
					}
					else
					{
						(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
							{
								CStr Error = "{} files in the manifest? This was unexpected"_f << Result.m_Manifest.m_Files.f_GetLen();
								CheckResultPromise.f_SetException(Auditor.f_Exception(Error));
							}
						;
					}
				}
				else
				{
					// The secret was removed while the file was transferred. Ooops.
					(*pCleanupFile)->f_Destroy() > [=](TCAsyncResult<void> &&)
						{
							CheckResultPromise.f_SetException(Auditor.f_Exception("Secret '{}' removed while the secret file was uploaded"_f << _ID));
						}
					;
				}
			}
		;

#if DMibConfig_Tests_Enable
		if (auto *pPromise = This.mp_UploadInitialized.f_FindEqual(_FileName))
		{
			pPromise->f_SetResult();	// Tell the test the transfer has been initialized (SaveSecret has been set so we know if it has changed when the transfer completes)
			This.mp_UploadInitialized.f_Remove(pPromise);
		}
#endif
		co_return g_ActorFunctor
			(
				g_ActorSubscription / [this, NewFileName, AllowDestroy = g_AllowWrongThreadDestroy]
				{
					auto &This = *m_pThis;
					This.mp_Uploads.f_Remove(NewFileName);
				}
			)
			/ [CheckResultFuture = fg_Move(CheckResultPromise.m_Future), AllowDestroy = g_AllowWrongThreadDestroy]() mutable -> TCFuture<void>
			{
				co_return co_await fg_Move(CheckResultFuture);
			}
		;
	}
 }
