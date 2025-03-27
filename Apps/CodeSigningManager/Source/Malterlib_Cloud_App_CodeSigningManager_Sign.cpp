// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Malterlib_Cloud_App_CodeSigningManager.h"

#include <Mib/Concurrency/LogError>
#include <Mib/Cryptography/SignFiles>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/Base64>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/File/File>

namespace NMib::NCloud::NCodeSigningManager
{
	namespace
	{
		struct CSigningDirectories
		{
			CStr m_InputDirectory;
		};

		constexpr EFileAttrib gc_FilePermissions = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid;

		TCFuture<CSigningDirectories> fg_PrepareSigningDirectories(CStr _WorkingDirectory)
		{
			auto BlockingActor = fg_BlockingActor();

			auto InputDirectory = _WorkingDirectory / "Input";

			co_await
				(
					g_Dispatch(BlockingActor) / [WorkingDirectory = _WorkingDirectory]() -> TCFuture<void>
					{
						auto Capture = co_await (g_CaptureExceptions % "Failed to prepare signing directories");

						// Create only the working directory, not the input directory
						// CFileTransferReceive will create the input directory
						CFile::fs_CreateDirectory(WorkingDirectory);

						co_return {};
					}
				)
			;

			CSigningDirectories Directories;
			Directories.m_InputDirectory = fg_Move(InputDirectory);

			co_return fg_Move(Directories);
		}

		TCFuture<void> fg_CleanupSigningDirectory(CStr _Path)
		{
			if (_Path.f_IsEmpty())
				co_return {};

			auto BlockingActor = fg_BlockingActor();

			co_await
				(
					g_Dispatch(BlockingActor) / [Path = fg_Move(_Path)]() -> TCFuture<void>
					{
						auto Capture = co_await (g_CaptureExceptions % "Failed to cleanup signing directory");

						if (CFile::fs_FileExists(Path))
							CFile::fs_DeleteDirectoryRecursive(Path);

						co_return {};
					}
				)
			;

			co_return {};
		}

		TCFuture<TCVector<CFile::CFoundFile>> fg_FindFiles(CStr _Directory)
		{
			auto BlockingActor = fg_BlockingActor();

			auto Result = co_await
				(
					g_Dispatch(BlockingActor) /
					[Directory = fg_Move(_Directory)]() -> TCFuture<TCVector<CFile::CFoundFile>>
					{
						auto Capture = co_await (g_CaptureExceptions % "Failed to enumerate directory");

						CFile::CFindFilesOptions Options(Directory / "*", false);
						co_return CFile::fs_FindFiles(Options);
					}
				)
			;

			co_return fg_Move(Result);
		}
	}

	TCFuture<NCloud::CCodeSigningManager::CSignFiles::CResult> CCodeSigningManagerActor::CCodeSigningInterface::f_SignFiles(NCloud::CCodeSigningManager::CSignFiles _Params)
	{
		auto pThis = m_pThis;

		auto CallingHostInfo = fg_GetCallingHostInfo();
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_State.f_Auditor("CodeSigning/SignExecutable", CallingHostInfo);

		TCVector<CSigningCertKey> FoundSigningCertKeys;

		for (auto &SigningCert : pThis->mp_SigningCerts)
		{
			auto &Key = SigningCert.f_GetKey();

			if (_Params.m_Authority && Key.m_Authority != *_Params.m_Authority)
				continue;

			if (_Params.m_SigningCert && Key.m_Name != *_Params.m_SigningCert)
				continue;

			FoundSigningCertKeys.f_Insert(Key);
		}

		if (FoundSigningCertKeys.f_IsEmpty())
			co_return Auditor.f_Exception("No signing cert found for {}: {}"_f << _Params.m_Authority << _Params.m_SigningCert);

		if (FoundSigningCertKeys.f_GetLen() != 1)
			co_return Auditor.f_Exception("Found {} matching signing certificates for {}: {}"_f << _Params.m_Authority << _Params.m_SigningCert);

		if (!_Params.m_FilesGenerator.f_IsValid())
			co_return Auditor.f_Exception("No executable data supplied");

		if (_Params.m_QueueSize > gc_IdealNetworkQueueSize)
			co_return Auditor.f_Exception("Queue size larger than maximum allowed");

		CSigningCertKey SigningKey = FoundSigningCertKeys[0];

		CSigningCert *pSigningCert;
		auto OnResumeSigning = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pSigningCert = pThis->mp_SigningCerts.f_FindEqual(SigningKey);
					if (!pSigningCert)
						return Auditor.f_Exception("Signing certificate was deleted: {}: {}"_f << SigningKey.m_Authority << SigningKey.m_Name);

					return {};
				}
			)
		;

		auto PrivateKey = co_await (pThis->fp_SigningCert_FetchPrivateKey(SigningKey) % "Failed to fetch private key" % Auditor);

		CStr JobID = fg_RandomID(pThis->mp_SignSessions);
		pThis->mp_SignSessions[JobID];

		CSignSession *pSession;
		auto OnResumeSession = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pSession = pThis->mp_SignSessions.f_FindEqual(JobID);
					if (!pSession)
						return Auditor.f_Exception("Signing session was aborted");

					return {};
				}
			)
		;

		pSession->m_WorkDirectory = pThis->mp_State.m_RootDirectory / "CodeSigningJobs" / JobID;

		auto SessionSubscription = g_ActorSubscription / [pThis, JobID, WorkingDirectory = pSession->m_WorkDirectory]() -> TCFuture<void>
			{
				TCFutureVector<void> DestroyTasks;
				if (auto *pSession = pThis->mp_SignSessions.f_FindEqual(JobID))
				{
					if (pSession->m_FileTransferReceive)
						fg_Move(pSession->m_FileTransferReceive).f_Destroy() > DestroyTasks;
					pThis->mp_SignSessions.f_Remove(JobID);
				}

				co_await fg_AllDoneWrapped(DestroyTasks);

				co_await fg_CleanupSigningDirectory(WorkingDirectory);

				co_return {};
			}
		;

		auto DirectoriesResult = co_await fg_PrepareSigningDirectories(pSession->m_WorkDirectory).f_Wrap();
		if (!DirectoriesResult)
		{
			pThis->mp_SignSessions.f_Remove(JobID);
			co_return Auditor.f_Exception(DirectoriesResult.f_GetExceptionStr());
		}

		pSession->m_InputDirectory = DirectoriesResult->m_InputDirectory;
		pSession->m_FileTransferReceive = fg_ConstructActor<NCloud::CFileTransferReceive>(pSession->m_InputDirectory, gc_FilePermissions, gc_FilePermissions);

		auto ReceiveFuture = pSession->m_FileTransferReceive
			(
				&NCloud::CFileTransferReceive::f_ReceiveFiles
				, NCloud::CFileTransferSendDownloadFile::fs_TranslateGenerator<NCloud::CFileTransferSendDownloadFile>(fg_Move(_Params.m_FilesGenerator))
				, _Params.m_QueueSize ? _Params.m_QueueSize : gc_IdealNetworkQueueSize
				, NCloud::CFileTransferReceive::EReceiveFlag_FailOnExisting
			)
			.f_Call()
		;

		auto SignFuture = fg_CallSafe
			(
				[
					InputDirectory = pSession->m_InputDirectory
					, ReceiveFuture = fg_Move(ReceiveFuture)
					, Auditor
					, SigningKey
					, Certificate = pSigningCert->m_Certificate
					, PrivateKey
					, TimestampOptions = pThis->mp_TimestampOptions
				] mutable -> TCFuture<CEJsonSorted>
				{
					auto ReceiveResult = co_await (fg_Move(ReceiveFuture) % "Failed to transfer executable" % Auditor);
					Auditor.f_Info("Signed executable transfer: {ns } bytes at {fe2} MB/s"_f << ReceiveResult.m_nBytes << (ReceiveResult.f_BytesPerSecond() / 1'000'000.0));

					auto Files = co_await fg_FindFiles(InputDirectory).f_Wrap();
					if (!Files)
						co_return Auditor.f_Exception(fg_Format("Failed to enumerate uploaded files: {}", Files.f_GetExceptionStr()));

					if (Files->f_IsEmpty())
						co_return Auditor.f_Exception("No executable received");

					if (Files->f_GetLen() > 1)
						co_return Auditor.f_Exception("Multiple files uploaded; only one root file / directory per request is supported");

					CStr ExecutablePath = (*Files)[0].m_Path;
					CStr ExecutableName = CFile::fs_GetFile((*Files)[0].m_Path);

					auto fGetSignatureJson = NCryptography::fg_SignFiles(ExecutablePath, fg_Move(PrivateKey), Certificate.f_ToInsecure(), NCryptography::EDigestType_SHA512, TimestampOptions);
					auto SignatureJson = co_await fGetSignatureJson();
					SignatureJson["AuthorityName"] = SigningKey.m_Authority;
					SignatureJson["CertificateName"] = SigningKey.m_Name;

					Auditor.f_Info("Signed files:\n{}", SignatureJson.f_ToString().f_Indent("    "));

					co_return fg_Move(SignatureJson);
				}
			)
		;

		NCloud::CCodeSigningManager::CSignFiles::CResult Result;
		Result.m_fGetSignature = g_ActorFunctor(fg_Move(SessionSubscription)) / [SignFuture = fg_Move(SignFuture), Auditor] mutable -> TCFuture<CEJsonSorted>
			{
				co_return co_await (fg_Move(SignFuture) % Auditor);
			}
		;

		Auditor.f_Info("Sign files prepared");

		co_return Result;
	}

	TCFuture<void> CCodeSigningManagerActor::fp_Publish()
	{
		co_await mp_CodeSigningInterface.f_Publish<NCloud::CCodeSigningManager>(mp_State.m_DistributionManager, this);
		co_return {};
	}
}
