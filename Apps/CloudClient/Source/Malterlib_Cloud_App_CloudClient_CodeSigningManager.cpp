// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Cloud/CodeSigningManager>
#include <Mib/Cloud/FileTransfer>
#include <Mib/CommandLine/TableRenderer>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/LogError>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/File/File>

#include "Malterlib_Cloud_App_CloudClient.h"

namespace NMib::NCloud::NCloudClient
{
	namespace
	{
		TCFuture<bool> fg_ValidateInputFile(CStr _Path)
		{
			auto BlockingActor = fg_BlockingActor();

			auto Result = co_await
				(
					g_Dispatch(BlockingActor) /
					[Path = fg_Move(_Path)]() -> TCFuture<bool>
					{
						auto Capture = co_await (g_CaptureExceptions % "Failed to validate input file");
						if (!CFile::fs_FileExists(Path))
							co_return false;
						co_return true;
					}
				)
				.f_Wrap()
			;

			if (!Result)
				co_return Result.f_GetException();

			co_return *Result;
		}
	}

	void CCloudClientAppActor::fp_CodeSigningManager_RegisterCommands(CDistributedAppCommandLineSpecification::CSection _Section)
	{
		_Section.f_RegisterCommand
			(
				{
					"Names"_o= _o["--code-signing-manager-sign-files"]
					, "Description"_o= "Sign files using the cloud code signing manager."
					, "Options"_o=
					{
						"CodeSigningHost?"_o=
						{
							"Names"_o= _o["--host"]
							, "Default"_o= ""
							, "Description"_o= "Limit signing to a specific code signing manager host ID."
						}
						, "Authority?"_o=
						{
							"Names"_o= _o["--authority"]
							, "Type"_o= ""
							, "Description"_o= "Name of the signing authority to use."
						}
						, "SigningCert?"_o=
						{
							"Names"_o= _o["--signing-cert"]
							, "Type"_o= ""
							, "Description"_o= "Name of the signing certificate to use."
						}
						, "Input"_o=
						{
							"Names"_o= _o["--input"]
							, "Type"_o= ""
							, "Description"_o= "Path to the executable to sign."
						}
						, "Output?"_o=
						{
							"Names"_o= _o["--output", "-o"]
							, "Type"_o= ""
							, "Description"_o= "Path to output the signature JSON. Defaults to <input>.signature.json."
						}
						, "Stdout?"_o=
						{
							"Names"_o= _o["--stdout"]
							, "Default"_o= false
							, "Description"_o= "Output signature to stdout instead of writing to a file."
						}
						, "QueueSize?"_o=
						{
							"Names"_o= _o["--queue-size"]
							, "Default"_o= int64(gc_IdealNetworkQueueSize)
							, "Description"_o= "Amount of data to keep in flight during transfer."
						}
						, "CurrentDirectory?"_o=
						{
							"Names"_o= _o[]
							, "Default"_o= CFile::fs_GetCurrentDirectory()
							, "Hidden"_o= true
							, "Description"_o= "Internal option used to propagate current directory."
						}
					}
				}
				, [this](CEJsonSorted &&_Params, NStorage::TCSharedPointer<CCommandLineControl> &&_pCommandLine)
				{
					return fp_CommandLine_CodeSigningManager_SignFiles(fg_Move(_Params), fg_Move(_pCommandLine));
				}
				, EDistributedAppCommandFlag_WaitForRemotes
			)
		;
	}

	TCFuture<void> CCloudClientAppActor::fp_CodeSigningManager_SubscribeToServers()
	{
		if (!mp_CodeSigningManagers.m_Actors.f_IsEmpty())
			co_return {};

		DMibLogWithCategory(Malterlib/Cloud/CloudClient, Info, "Subscribing to code signing managers");

		auto Subscription = co_await mp_State.m_TrustManager
			->f_SubscribeTrustedActors<CCodeSigningManager>(CCodeSigningManager::EProtocolVersion_SupportExecutableSigning)
			.f_Wrap()
		;

		if (!Subscription)
		{
			DMibLogWithCategory(Malterlib/Cloud/CloudClient, Error, "Failed to subscribe to code signing managers: {}", Subscription.f_GetExceptionStr());
			co_return Subscription.f_GetException();
		}

		mp_CodeSigningManagers = fg_Move(*Subscription);

		if (mp_CodeSigningManagers.m_Actors.f_IsEmpty())
			co_return DMibErrorInstance("Not connected to any code signing managers, or they are not trusted for 'com.malterlib/Cloud/CodeSigningManager'.");

		co_return {};
	}

	TCFuture<TCVector<TCTrustedActor<CCodeSigningManager>>> CCloudClientAppActor::fp_CodeSigningManager_GetManagers(CStr _Host)
	{
		if (!_Host.f_IsEmpty())
		{
			CStr Error;
			auto *pManager = mp_CodeSigningManagers.f_GetOneActor(_Host, Error);
			if (!pManager)
				co_return DMibErrorInstance("Error selecting code signing manager for host '{}': {}"_f << _Host << Error);

			co_return {*pManager};
		}

		TCVector<TCTrustedActor<CCodeSigningManager>> Managers;
		for (auto &Actor : mp_CodeSigningManagers.m_Actors)
			Managers.f_Insert(Actor);

		if (Managers.f_IsEmpty())
			co_return DMibErrorInstance("No code signing managers connected.");

		co_return fg_Move(Managers);
	}

	TCFuture<uint32> CCloudClientAppActor::fp_CommandLine_CodeSigningManager_SignFiles(CEJsonSorted const _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Host = _Params["CodeSigningHost"].f_String();
		uint64 QueueSize = _Params["QueueSize"].f_Integer();
		if (QueueSize < 128 * 1024)
			QueueSize = 128 * 1024;

		CStr CurrentDirectory = _Params["CurrentDirectory"].f_String();
		bool bOutputToStdout = _Params["Stdout"].f_Boolean();

		CStr InputPath = CFile::fs_GetExpandedPath(_Params["Input"].f_String(), CurrentDirectory);
		CStr OutputPath;
		if (auto *pOutput = _Params.f_GetMember("Output"))
			OutputPath = CFile::fs_GetExpandedPath(pOutput->f_String(), CurrentDirectory);
		else
			OutputPath = "{}.signature.json"_f << InputPath;
		if (InputPath.f_IsEmpty())
			co_return DMibErrorInstance("Input path must be specified.");

		auto FileExists = co_await fg_ValidateInputFile(InputPath).f_Wrap();
		if (!FileExists)
			co_return DMibErrorInstance("Input file '{}' does not exist.", InputPath);

		co_await fp_CodeSigningManager_SubscribeToServers().f_Timeout(mp_Timeout, "Timed out waiting for subscriptions for code signing managers");

		auto Managers = co_await fp_CodeSigningManager_GetManagers(Host);

		if (Host.f_IsEmpty() && Managers.f_GetLen() > 1)
			co_return DMibErrorInstance("Multiple code signing managers available. Specify --host to select one.");

		auto &TrustedActor = Managers[0];
		auto const &HostInfo = TrustedActor.m_TrustInfo.m_HostInfo;

		TCActor<CFileTransferSend> UploadSend = fg_Construct(InputPath, QueueSize);
		auto DestroyUploadSend = co_await fg_AsyncDestroy(UploadSend);

		auto SendFiles = co_await UploadSend.f_Bind<&CFileTransferSend::f_SendFiles>(CFileTransferSend::CSendFilesOptions{.m_bIncludeRootDirectoryName = true});

		CCodeSigningManager::CSignFiles Request;

		if (auto *pValue = _Params.f_GetMember("Authority"))
			Request.m_Authority = pValue->f_String();

		if (auto *pValue = _Params.f_GetMember("SigningCert"))
			Request.m_SigningCert = pValue->f_String();

		Request.m_QueueSize = QueueSize;

		TCAsyncGeneratorWithID<CCodeSigningManager::CDownloadFile> FilesGenerator
			= CFileTransferSendDownloadFile::fs_TranslateGenerator<CCodeSigningManager::CDownloadFile>(fg_Move(SendFiles.m_FilesGenerator))
		;

		FilesGenerator.f_SetSubscription(fg_Move(SendFiles.m_Subscription));
		Request.m_FilesGenerator = fg_Move(FilesGenerator);

		auto UploadFuture = fg_Move(SendFiles.m_Result);

		auto SignResult = co_await
			(
				TrustedActor.m_Actor
				.f_CallActor(&CCodeSigningManager::f_SignFiles)(fg_Move(Request))
				.f_Timeout(mp_Timeout, "Timed out waiting for code signing manager response")
				% ("Failed to sign executable on '{}'"_f << HostInfo.f_GetDesc())
			)
		;

		auto DestroySignResult = co_await fg_AsyncDestroy
			(
				[&] -> TCFuture<void>
				{
					auto fGetSignature = fg_Move(SignResult.m_fGetSignature);
					co_await fg_Move(fGetSignature).f_Destroy();
					co_return {};
				}
			)
		;

		auto UploadTransfer = co_await (fg_Move(UploadFuture) % ("Failed to upload executable to '{}'"_f << HostInfo.f_GetDesc()));

		auto Signature = co_await (SignResult.m_fGetSignature() % ("Failed to get sigtature from '{}'"_f << HostInfo.f_GetDesc()));

		auto AnsiEncoding = _pCommandLine->f_AnsiEncoding();
		auto FormatFlags = AnsiEncoding.f_Flags();
		CStr ExecutableName = CFile::fs_GetFile(InputPath);

		*_pCommandLine %= "Signed executable on '{}':\n"_f << HostInfo.f_GetDescColored(FormatFlags);
		*_pCommandLine %= "  Upload: {ns } bytes at {fe2} MB/s\n"_f << UploadTransfer.m_nBytes << (UploadTransfer.f_BytesPerSecond() / 1'000'000.0);

		CStr SignatureJson = Signature.f_ToString();

		if (bOutputToStdout)
		{
			co_await _pCommandLine->f_StdErr(""); // Synchronize
			co_await _pCommandLine->f_StdOut(SignatureJson);
		}
		else
		{
			auto BlockingActor = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActor) / [OutputPath, SignatureJson]()
					{
						CFile::fs_CreateDirectoryForFile(OutputPath);
						CFile::fs_WriteStringToFile(OutputPath, SignatureJson);
					}
					% ("Failed to write signature to '{}'"_f << OutputPath)
				)
			;

			*_pCommandLine %= "  Output: {}\n"_f << OutputPath;
			co_await _pCommandLine->f_StdErr(""); // Synchronize
		}

		co_return 0;
	}
}
