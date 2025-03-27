// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppLaunchHelper>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Cloud/App/KeyManager>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/SecretsManagerUpload>
#include <Mib/Cloud/SecretsManagerDownload>
#include <Mib/Cloud/App/SecretsManager>
#include <Mib/Cloud/CodeSigningManager>
#include <Mib/Cloud/App/CodeSigningManager>
#include <Mib/Cloud/FileTransfer>
#include <Mib/Cryptography/RandomID>
#include <Mib/Cryptography/Certificate>
#include <Mib/Cryptography/SignFiles>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Test/Exception>
#include <Mib/File/DirectorySync>
#include <Mib/File/DirectoryManifest>

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NProcess;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NCloud;
using namespace NMib::NStorage;
using namespace NMib::NAtomic;
using namespace NMib::NEncoding;
using namespace NMib::NNetwork;
using namespace NMib::NTest;

static fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;

namespace NCodeSigningManagerTests
{
	TCFuture<CStr> fg_LaunchWithStdIn
		(
			CStr _Executable
			, TCVector<CStr> _Params
			, CStr _WorkingDir
			, TCVector<CStr> _StdInLines
			, fp64 _Timeout
		)
	{
		struct CProcessWithStdinResult
		{
			CStr m_AllOutput;
			CStr m_StdOut;
			CStr m_StdErr;
		};

		TCSharedPointer<CProcessWithStdinResult> pResult = fg_Construct();

		TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();
		TCPromise<void> LaunchFinished;

		CProcessLaunchActor::CLaunch Launch(CProcessLaunchParams::fs_LaunchExecutable(_Executable, _Params, _WorkingDir, {}));

		if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
		{
			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_Error | CProcessLaunchActor::ELogFlag_StdErr | CProcessLaunchActor::ELogFlag_StdOut;
			Launch.m_LogName = CFile::fs_GetFile(_Executable);
		}

		Launch.m_Params.m_fOnOutput = [LaunchActor, pResult, StdInBuffer = CStr(), nStdInIndex = mint(0), _StdInLines](EProcessLaunchOutputType _OutputType, CStr const &_Output) mutable
			{
				pResult->m_AllOutput += _Output;
				if (_OutputType == EProcessLaunchOutputType_StdErr)
					pResult->m_StdErr += _Output;
				else if (_OutputType == EProcessLaunchOutputType_StdOut)
					pResult->m_StdOut += _Output;

				// Check for password prompts
				StdInBuffer += _Output;

				if
				(
					(StdInBuffer.f_Find("Password        :") >= 0 || StdInBuffer.f_Find("Password (again):") >= 0 || StdInBuffer.f_Find("Password:") >= 0)
					&& nStdInIndex < _StdInLines.f_GetLen()
				)
				{
					StdInBuffer.f_Clear();
					LaunchActor(&CProcessLaunchActor::f_SendStdIn, _StdInLines[nStdInIndex] + "\n").f_DiscardResult();
					++nStdInIndex;
				}
			}
		;
		Launch.m_bWholeLineOutput = false;

		Launch.m_Params.m_fOnStateChange = [LaunchFinished, pResult](CProcessLaunchStateChangeVariant const &_State, fp64)
			{
				if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
				{
					if (_State.f_Get<EProcessLaunchState_Exited>() != 0)
						LaunchFinished.f_SetException(DMibErrorInstance(pResult->m_AllOutput));
					else
						LaunchFinished.f_SetResult();
				}
				else if (_State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
				{
					LaunchFinished.f_SetException(DMibErrorInstance(_State.f_Get<EProcessLaunchState_LaunchFailed>()));
				}
			}
		;

		auto AsyncDestroyLaunchActor = co_await fg_AsyncDestroy(LaunchActor);
		auto LaunchSubscription = co_await LaunchActor(&CProcessLaunchActor::f_Launch, Launch, fg_CurrentActor());
		co_await LaunchFinished.f_MoveFuture().f_Timeout(_Timeout, "Process timed out");

		co_return fg_Move(pResult->m_StdOut);
	}

	// Helper function for signing files directly via the CCodeSigningManager interface
	// This is the same logic as in CloudClient but exposed as a reusable async function
	TCFuture<CEJsonSorted> fg_SignFilesViaInterface
		(
			TCDistributedActor<CCodeSigningManager> _CodeSigningManager
			, CStr _InputPath
			, TCOptional<CStr> _Authority
			, TCOptional<CStr> _SigningCert
			, uint64 _QueueSize
			, fp64 _Timeout
		)
	{
		TCActor<CFileTransferSend> UploadSend = fg_Construct(_InputPath, _QueueSize);
		auto DestroyUploadSend = co_await fg_AsyncDestroy(UploadSend);

		auto SendFiles = co_await UploadSend.f_Bind<&CFileTransferSend::f_SendFiles>(CFileTransferSend::CSendFilesOptions{.m_bIncludeRootDirectoryName = true});

		CCodeSigningManager::CSignFiles Request;
		Request.m_Authority = _Authority;
		Request.m_SigningCert = _SigningCert;
		Request.m_QueueSize = _QueueSize;

		TCAsyncGeneratorWithID<CCodeSigningManager::CDownloadFile> FilesGenerator
			= CFileTransferSendDownloadFile::fs_TranslateGenerator<CCodeSigningManager::CDownloadFile>(fg_Move(SendFiles.m_FilesGenerator))
		;

		FilesGenerator.f_SetSubscription(fg_Move(SendFiles.m_Subscription));
		Request.m_FilesGenerator = fg_Move(FilesGenerator);

		auto UploadFuture = fg_Move(SendFiles.m_Result);

		auto SignResult = co_await
			(
				_CodeSigningManager
				.f_CallActor(&CCodeSigningManager::f_SignFiles)(fg_Move(Request))
				.f_Timeout(_Timeout, "Timed out waiting for code signing manager response")
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

		co_await (fg_Move(UploadFuture) % "Failed to upload file");

		auto Signature = co_await (SignResult.m_fGetSignature() % "Failed to get signature");

		co_return fg_Move(Signature);
	}

	// Helper function to create a user with password auth and import to SecretsManagers
	TCFuture<CStr> fg_CreateAndImportUser
		(
			CStr _ManagementManagerExe
			, CStr _ManagementManagerDirectory
			, TCDistributedActor<CDistributedActorTrustManagerInterface> _ManagementTrust
			, TCVector<TCDistributedActor<CDistributedActorTrustManagerInterface>> _SecretsManagerTrusts
			, CStr _UserName
			, CStr _Password
			, fp64 _Timeout
		)
	{
		CStr UserID = fg_RandomID();

		// Create user on Management CodeSigningManager
		co_await _ManagementTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddUser)(UserID, _UserName);

		// Register password authentication factor
		auto RegisterResult = co_await fg_LaunchWithStdIn
			(
				_ManagementManagerExe + CFile::mc_ExecutableExtension
				, {"--trust-user-add-authentication-factor", "--authentication-factor", "Password", UserID}
				, _ManagementManagerDirectory
				, {_Password, _Password} // Enter twice for confirmation
				, _Timeout
			)
		;

		// Export user (without private data)
		CStr ExportedUser = co_await fg_ExportUser(_ManagementTrust, UserID, false);

		// Import into all SecretsManagers
		for (auto &SecretsManagerTrust : _SecretsManagerTrusts)
			co_await fg_ImportUser(SecretsManagerTrust, ExportedUser);

		co_return UserID;
	}

	class CCodeSigningManager_Tests : public NMib::NTest::CTest
	{
	public:
		void f_DoTests()
		{
			DMibTestSuite("General") -> TCFuture<void>
			{
				auto CaptureExceptions = co_await g_CaptureExceptions;

				CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
				CStr RootDirectory = ProgramDirectory + "/CodeSigningManagerTests";
				fg_TestAddCleanupPath(RootDirectory);

				bool bEnableLogs = !!(fg_TestReportFlags() & ETestReportFlag_EnableLogs);

				TCVector<CStr> LogParams;
				if (bEnableLogs)
					LogParams = {"--log-to-stderr", "--no-color"};

				CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 10.0);

				// Clean up previous test run
				for (mint i = 0; i < 5; ++i)
				{
					try
					{
						if (CFile::fs_FileExists(RootDirectory))
							CFile::fs_DeleteDirectoryRecursive(RootDirectory);
						break;
					}
					catch (NFile::CExceptionFile const &)
					{
					}
				}

				CFile::fs_CreateDirectory(RootDirectory);

				// Setup trust manager
				CTrustManagerTestHelper TrustManagerState;
				TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
				auto AsyncDestroyTrustManager = co_await fg_AsyncDestroy(TrustManager);

				CStr TestHostID = co_await TrustManager(&CDistributedActorTrustManager::f_GetHostID);
				CTrustedSubscriptionTestHelper Subscriptions{TrustManager};

				CDistributedActorTrustManager_Address ServerAddress;
				ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
				co_await TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress);

				CDistributedApp_LaunchHelperDependencies Dependencies;
				Dependencies.m_Address = ServerAddress.m_URL;
				Dependencies.m_TrustManager = TrustManager;
				Dependencies.m_DistributionManager = co_await TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager);

				NMib::NConcurrency::CDistributedActorSecurity Security;
				Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CSecretsManager::mc_pDefaultNamespace);
				Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CCodeSigningManager::mc_pDefaultNamespace);
				co_await Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security);

				TCActor<CDistributedApp_LaunchHelper> LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, bEnableLogs);
				auto AsyncDestroyLaunchHelper = co_await fg_AsyncDestroy(LaunchHelper);

				// Setup KeyManager
				CStr KeyManagerDirectory = RootDirectory + "/KeyManager";
				CFile::fs_CreateDirectory(KeyManagerDirectory);
				CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/KeyManager", KeyManagerDirectory, nullptr);

				// Setup CloudClient for integration tests
				CStr CloudClientDirectory = RootDirectory + "/MalterlibCloud";
				CFile::fs_CreateDirectory(CloudClientDirectory);
				CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/MalterlibCloud", CloudClientDirectory, nullptr);

				// Setup SecretsManagers (2 instances)
				mint nSecretsManagers = 2;
				TCVector<CStr> SecretsManagerDirectories;
				for (mint i = 0; i < nSecretsManagers; ++i)
				{
					CStr SecretsManagerName = "SecretsManager{sf0,sl2}"_f << i;
					CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
					CFile::fs_CreateDirectory(SecretsManagerDirectory);
					SecretsManagerDirectories.f_InsertLast(SecretsManagerDirectory);
				}

				// Setup CodeSigningManagers (2 instances: signing and management)
				CStr SigningCodeSigningManagerDirectory = RootDirectory + "/CodeSigningManager_Signing";
				CStr ManagementCodeSigningManagerDirectory = RootDirectory + "/CodeSigningManager_Management";
				CFile::fs_CreateDirectory(SigningCodeSigningManagerDirectory);
				CFile::fs_CreateDirectory(ManagementCodeSigningManagerDirectory);
				CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/CodeSigningManager", SigningCodeSigningManagerDirectory, nullptr);
				CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/CodeSigningManager", ManagementCodeSigningManagerDirectory, nullptr);

				// Launch SecretsManagers
				TCVector<CDistributedApp_LaunchInfo> SecretsManagerLaunches;
				for (mint i = 0; i < nSecretsManagers; ++i)
				{
					CStr SecretsManagerName = "SecretsManager{sf0,sl2}"_f << i;
					auto Launch = co_await LaunchHelper
						(
							&CDistributedApp_LaunchHelper::f_LaunchInProcess
							, SecretsManagerName
							, SecretsManagerDirectories[i]
							, &fg_ConstructApp_SecretsManager
							, TCVector<CStr>{}
						)
					;
					SecretsManagerLaunches.f_Insert(fg_Move(Launch));
				}

				// Launch KeyManager
				auto KeyManagerLaunch = co_await LaunchHelper
					(
						&CDistributedApp_LaunchHelper::f_LaunchInProcess
						, "KeyManager"
						, KeyManagerDirectory
						, &fg_ConstructApp_KeyManager
						, TCVector<CStr>{}
					)
				;
				DMibExpect(KeyManagerLaunch.m_HostID, !=, "");

				// Setup KeyManager password
				auto pKeyManagerTrust = KeyManagerLaunch.m_pTrustInterface;
				auto &KeyManagerTrust = *pKeyManagerTrust;
				CStr KeyManagerHostID = KeyManagerLaunch.m_HostID;

				CDistributedActorTrustManager_Address KeyManagerServerAddress;
				KeyManagerServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/KeyManager.sock"_f << KeyManagerDirectory);
				co_await KeyManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(KeyManagerServerAddress);

				// Provide password to KeyManager
				{
					TCActor<CProcessLaunchActor> KeyManagerCommandLine = fg_Construct();
					auto AsyncDestroyKM = co_await fg_AsyncDestroy(KeyManagerCommandLine);

					CProcessLaunchActor::CSimpleLaunch LaunchParams{KeyManagerDirectory + "/KeyManager", {"--provide-password"}};
					LaunchParams.m_DestructFlags = EProcessLaunchCloseFlag_BlockOnExit;
					LaunchParams.m_ToLog = CProcessLaunchActor::ELogFlag_All;
					if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
						LaunchParams.m_ToLog |= CProcessLaunchActor::ELogFlag_AdditionallyOutputToStdErr;

					TCPromise<void> LaunchedPromise;
					TCPromise<void> ExitedPromise;

					LaunchParams.m_Params.m_fOnStateChange = [LaunchedPromise, ExitedPromise](CProcessLaunchStateChangeVariant const &_State, fp64)
						{
							switch (_State.f_GetTypeID())
							{
							case EProcessLaunchState_Launched:
								LaunchedPromise.f_SetResult();
								break;
							case EProcessLaunchState_LaunchFailed:
								LaunchedPromise.f_SetException(DMibErrorInstance(_State.f_Get<EProcessLaunchState_LaunchFailed>()));
								break;
							case EProcessLaunchState_Exited:
								{
									auto ExitStatus = _State.f_Get<EProcessLaunchState_Exited>();
									if (ExitStatus)
										ExitedPromise.f_SetException(DMibErrorInstance(fg_Format("KeyManager password setup failed: Status {}", ExitStatus)));
									else
										ExitedPromise.f_SetResult();
								}
								break;
							}
						}
					;
					auto LaunchSubscription = co_await KeyManagerCommandLine(&CProcessLaunchActor::f_Launch, fg_Move(LaunchParams), fg_CurrentActor());
					co_await LaunchedPromise.f_MoveFuture().f_Timeout(g_Timeout, "KeyManager launch timed out");
					co_await KeyManagerCommandLine(&CProcessLaunchActor::f_SendStdIn, "Password\n");
					co_await ExitedPromise.f_MoveFuture().f_Timeout(g_Timeout, "KeyManager password setup timed out");
				}

				// Launch CodeSigningManagers
				auto SigningCodeSigningManagerLaunch = co_await LaunchHelper
					(
						&CDistributedApp_LaunchHelper::f_LaunchInProcess
						, "CodeSigningManager_Signing"
						, SigningCodeSigningManagerDirectory
						, &fg_ConstructApp_CodeSigningManager
						, TCVector<CStr>{}
					)
				;
				DMibExpect(SigningCodeSigningManagerLaunch.m_HostID, !=, "");

				auto ManagementCodeSigningManagerLaunch = co_await LaunchHelper
					(
						&CDistributedApp_LaunchHelper::f_LaunchInProcess
						, "CodeSigningManager_Management"
						, ManagementCodeSigningManagerDirectory
						, &fg_ConstructApp_CodeSigningManager
						, TCVector<CStr>{}
					)
				;
				DMibExpect(ManagementCodeSigningManagerLaunch.m_HostID, !=, "");

				// Setup trust relationships and permissions
				static auto constexpr c_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;

				auto fPermissionsAdd = [](auto &&_HostID, auto &&_Permissions, CStr _UserID = "")
					{
						return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, _UserID}, _Permissions, c_WaitForSubscriptions};
					}
				;

				auto fNamespaceHosts = [](auto &&_Namespace, auto &&_Hosts)
					{
						return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, c_WaitForSubscriptions};
					}
				;

				// Collect SecretsManager info
				TCSet<CStr> AllSecretsManagerHosts;
				TCMap<CStr, TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>>> AllSecretsManagerTrust;
				TCMap<CStr, CDistributedActorTrustManager_Address> AllSecretsManagerAddresses;

				// Setup listen sockets for SecretsManagers
				{
					TCFutureVector<void> ListenResults;
					mint iSecretsManager = 0;
					for (auto &SecretsManager : SecretsManagerLaunches)
					{
						AllSecretsManagerHosts[SecretsManager.m_HostID];
						AllSecretsManagerTrust[SecretsManager.m_HostID] = SecretsManager.m_pTrustInterface;

						CDistributedActorTrustManager_Address Address;
						Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/SecretsManager.sock"_f << SecretsManagerDirectories[iSecretsManager]);
						AllSecretsManagerAddresses[SecretsManager.m_HostID] = Address;

						SecretsManager.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(Address) > ListenResults;
						++iSecretsManager;
					}
					fg_CombineResults(co_await fg_AllDoneWrapped(ListenResults));
				}

				// Setup listen sockets for CodeSigningManagers
				CDistributedActorTrustManager_Address SigningCodeSigningManagerAddress;
				SigningCodeSigningManagerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/CodeSigningManager.sock"_f << SigningCodeSigningManagerDirectory);
				co_await SigningCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(SigningCodeSigningManagerAddress);

				CDistributedActorTrustManager_Address ManagementCodeSigningManagerAddress;
				ManagementCodeSigningManagerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/CodeSigningManager.sock"_f << ManagementCodeSigningManagerDirectory);
				co_await ManagementCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(ManagementCodeSigningManagerAddress);

				CStr SigningHostID = SigningCodeSigningManagerLaunch.m_HostID;
				CStr ManagementHostID = ManagementCodeSigningManagerLaunch.m_HostID;

				// Allow CodeSigningManager namespace on test helper TrustManager from both CodeSigningManagers
				co_await TrustManager
					(
						&CDistributedActorTrustManager::f_AllowHostsForNamespace
						, CCodeSigningManager::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(SigningHostID, ManagementHostID)
						, EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions
					)
				;

				// Connect SecretsManagers to KeyManager and setup permissions
				{
					TCFutureVector<void> SetupResults;

					for (auto &SecretsManagerHost : AllSecretsManagerHosts)
					{
						auto pSecretsManagerTrust = AllSecretsManagerTrust[SecretsManagerHost];
						auto &SecretsManagerTrust = *pSecretsManagerTrust;

						// Allow KeyManager namespace
						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(CKeyManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(KeyManagerHostID))
							)
							> SetupResults
						;

						// Connect SecretsManager to KeyManager
						TCPromise<void> Promise;
						KeyManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
							(
								CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{KeyManagerServerAddress}
							)
							> Promise / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
							{
								auto &SecretsManagerTrust = *pSecretsManagerTrust;
								SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
							}
						;
						Promise.f_MoveFuture() > SetupResults;

						// Grant KeyManager permissions on SecretsManager
						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fPermissionsAdd
									(
										KeyManagerHostID
										, TCMap<CStr, CPermissionRequirements>
										{
											{"SecretsManager/CommandAll", {}}
										}
									)
							)
							> SetupResults
						;
					}

					fg_CombineResults(co_await fg_AllDoneWrapped(SetupResults));
				}

				// Setup password authentication for the Management CodeSigningManager
				// Two users are created: one for authority management, one for signing cert management
				CStr AuthorityPassword = "AuthorityPassword123";
				CStr SigningCertPassword = "SigningCertPassword123";
				CStr ManagementManagerExe = ManagementCodeSigningManagerDirectory + "/CodeSigningManager";

				TCVector<TCDistributedActor<CDistributedActorTrustManagerInterface>> SecretsManagerTrusts;
				for (auto &SecretsManager : SecretsManagerLaunches)
					SecretsManagerTrusts.f_InsertLast(SecretsManager.m_pTrustInterface->f_GetActor());

				CStr AuthorityUserID = co_await fg_CreateAndImportUser
					(
						ManagementManagerExe
						, ManagementCodeSigningManagerDirectory
						, ManagementCodeSigningManagerLaunch.m_pTrustInterface->f_GetActor()
						, SecretsManagerTrusts
						, "AuthorityManager"
						, AuthorityPassword
						, g_Timeout
					)
				;

				CStr SigningCertUserID = co_await fg_CreateAndImportUser
					(
						ManagementManagerExe
						, ManagementCodeSigningManagerDirectory
						, ManagementCodeSigningManagerLaunch.m_pTrustInterface->f_GetActor()
						, SecretsManagerTrusts
						, "SigningCertManager"
						, SigningCertPassword
						, g_Timeout
					)
				;

				// Connect CodeSigningManagers to SecretsManagers and setup permissions
				{
					TCFutureVector<void> SetupResults;

					for (auto &SecretsManagerHost : AllSecretsManagerHosts)
					{
						auto pSecretsManagerTrust = AllSecretsManagerTrust[SecretsManagerHost];
						auto &SecretsManagerTrust = *pSecretsManagerTrust;
						auto Address = AllSecretsManagerAddresses[SecretsManagerHost];

						// Connect Signing CodeSigningManager to SecretsManager
						{
							TCPromise<void> Promise;
							SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
								(
									CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{Address}
								)
								> Promise / [=, pTrust = SigningCodeSigningManagerLaunch.m_pTrustInterface]
								(CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
								{
									pTrust->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
								}
							;
							Promise.f_MoveFuture() > SetupResults;
						}

						// Connect Management CodeSigningManager to SecretsManager
						{
							TCPromise<void> Promise;
							SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
								(
									CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{Address}
								)
								> Promise / [=, pTrust = ManagementCodeSigningManagerLaunch.m_pTrustInterface]
								(CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
								{
									pTrust->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
								}
							;
							Promise.f_MoveFuture() > SetupResults;
						}

						// Allow SecretsManager namespace on CodeSigningManagers
						SigningCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(CSecretsManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(SecretsManagerHost))
							)
							> SetupResults
						;

						ManagementCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(CSecretsManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(SecretsManagerHost))
							)
							> SetupResults
						;

						// Allow DistributedActorAuthentication namespace on Management CodeSigningManager from SecretsManagers (needed for password auth)
						ManagementCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(ICDistributedActorAuthentication::mc_pDefaultNamespace, fg_CreateSet<CStr>(SecretsManagerHost))
							)
							> SetupResults
						;

						// Setup permissions on SecretsManagers for CodeSigningManagers
						// Common permissions for both (no auth required)
						TCMap<CStr, CPermissionRequirements> CommonPermissions =
						{
							{"SecretsManager/Command/SubscribeToChanges", {}}
							, {"SecretsManager/Command/GetSecretProperties", {}}
							, {"SecretsManager/Command/GetSecret", {}}
						};

						// Signing Manager: direct access to signing cert private key (no auth)
						TCMap<CStr, CPermissionRequirements> SigningManagerPermissions = CommonPermissions;
						SigningManagerPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.authority#*/Tag/Public"] = {};
						SigningManagerPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Public"] = {};
						SigningManagerPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Private"] = {}; // Direct access for signing

						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(fPermissionsAdd(SigningHostID, SigningManagerPermissions))
							> SetupResults
						;

						// Management Manager: common permissions without auth (host only, no user)
						TCMap<CStr, CPermissionRequirements> ManagementManagerCommonPermissions = CommonPermissions;
						ManagementManagerCommonPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.authority#*/Tag/Public"] = {};
						ManagementManagerCommonPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Public"] = {};

						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(fPermissionsAdd(ManagementHostID, ManagementManagerCommonPermissions)) // No UserID for common permissions
							> SetupResults
						;

						// Management Manager: password auth required for private key access and writes
						CPermissionRequirements PasswordAuth;
						PasswordAuth.m_AuthenticationFactors.f_Insert(fg_CreateSet<CStr>("Password"));

						// Authority management user: authority operations
						TCMap<CStr, CPermissionRequirements> AuthorityUserPermissions;
						AuthorityUserPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.authority#*/Tag/Private"] = PasswordAuth;
						AuthorityUserPermissions["SecretsManager/Write/SemanticID/org.malterlib.codesign.authority#*/Tag/Private"] = PasswordAuth;
						AuthorityUserPermissions["SecretsManager/Write/SemanticID/org.malterlib.codesign.authority#*/Tag/Public"] = PasswordAuth;
						AuthorityUserPermissions["SecretsManager/Command/SetSecretProperties"] = PasswordAuth;

						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(fPermissionsAdd(ManagementHostID, AuthorityUserPermissions, AuthorityUserID))
							> SetupResults
						;

						// Signing cert management user: signing cert operations + read authority private (to sign certs with CA key)
						TCMap<CStr, CPermissionRequirements> SigningCertUserPermissions;
						SigningCertUserPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.authority#*/Tag/Private"] = PasswordAuth; // Needed to sign certs with CA key
						SigningCertUserPermissions["SecretsManager/Read/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Private"] = PasswordAuth;
						SigningCertUserPermissions["SecretsManager/Write/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Private"] = PasswordAuth;
						SigningCertUserPermissions["SecretsManager/Write/SemanticID/org.malterlib.codesign.signingcert#*/Tag/Public"] = PasswordAuth;
						SigningCertUserPermissions["SecretsManager/Command/SetSecretProperties"] = PasswordAuth;

						SecretsManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(fPermissionsAdd(ManagementHostID, SigningCertUserPermissions, SigningCertUserID))
							> SetupResults
						;
					}

					fg_CombineResults(co_await fg_AllDoneWrapped(SetupResults));
				}

				// Create test data
				CStr TestFilePath = RootDirectory + "/TestFile.txt";
				CFile::fs_WriteStringToFile(TestFilePath, "This is a test file for code signing.\nIt has multiple lines.\n");

				CStr AuthorityName = "TestAuthority";
				CStr SigningCertName = "TestSigningCert";

				auto fGetParams = [&](TCVector<CStr> const &_Params)
					{
						TCVector<CStr> Params = _Params;
						Params.f_Insert(LogParams);
						return Params;
					}
				;

				{
					DMibTestPath("Create Authority and SigningCert via Management Manager");

					CStr AuthLifetimeMinutes = "{}"_f << fg_Max((g_Timeout / 60.0).f_ToIntRound(), 1);

					// Create authority (requires password auth with AuthorityUserID)
					auto AuthorityResult = co_await fg_LaunchWithStdIn
						(
							ManagementCodeSigningManagerDirectory + "/CodeSigningManager" + CFile::mc_ExecutableExtension
							,
							{
								"--authority-create"
								, "--name"
								, AuthorityName
								, "--elliptic-curve-type"
								, "secp521r1"
								, "--authentication-user"
								, AuthorityUserID
								, "--authentication-lifetime", AuthLifetimeMinutes
							}
							, ManagementCodeSigningManagerDirectory
							, {AuthorityPassword, AuthorityPassword, AuthorityPassword, AuthorityPassword} // Write public and private * 2 managers
							, g_Timeout
						)
						.f_Wrap()
					;
					DMibAssertNoException(AuthorityResult.f_Access());

					// Create signing certificate (requires password auth with SigningCertUserID to sign with authority key)
					auto SigningCertResult = co_await fg_LaunchWithStdIn
						(
							ManagementCodeSigningManagerDirectory + "/CodeSigningManager" + CFile::mc_ExecutableExtension
							,
							{
								"--signing-cert-create"
								, "--signing-cert"
								, SigningCertName
								, "--authority"
								, AuthorityName
								, "--elliptic-curve-type"
								, "secp384r1"
								, "--authentication-user"
								, SigningCertUserID
								, "--authentication-lifetime"
								, AuthLifetimeMinutes
							}
							, ManagementCodeSigningManagerDirectory
							,
							{ // Read authority + Write public and private * 2 managers
								SigningCertPassword
								, SigningCertPassword
								, SigningCertPassword
								, SigningCertPassword
								, SigningCertPassword
							}
							, g_Timeout
						)
						.f_Wrap()
					;
					DMibAssertNoException(SigningCertResult.f_Access());
				}

				// Get the authority certificate for verification
				auto AuthorityInfoResult = co_await CProcessLaunchActor::fs_LaunchSimple
					(
						CProcessLaunchActor::CSimpleLaunch
						{
							ManagementCodeSigningManagerDirectory + "/CodeSigningManager" + CFile::mc_ExecutableExtension
							, fGetParams({"--authority-info", "--name", AuthorityName})
							, ManagementCodeSigningManagerDirectory
							, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
						}
					)
					.f_Wrap()
				;
				DMibAssertNoException(AuthorityInfoResult.f_Access());

				CEJsonSorted AuthorityInfo = CEJsonSorted::fs_FromString(AuthorityInfoResult->f_GetStdOut());
				CByteVector CACertificate = AuthorityInfo["Certificate"].f_Binary();
				DMibExpectTrue(CACertificate.f_GetLen() > 0);

				{
					DMibTestPath("Sign File via Direct Interface");

					// Subscribe to the Signing CodeSigningManager
					auto CodeSigningManagerActor = co_await Subscriptions.f_SubscribeFromHostAsync<CCodeSigningManager>(SigningHostID);

					// Sign the test file directly via the interface
					auto SignatureJson = co_await fg_SignFilesViaInterface
						(
							CodeSigningManagerActor
							, TestFilePath
							, AuthorityName
							, SigningCertName
							, gc_IdealNetworkQueueSize
							, g_Timeout
						)
					;

					// Verify signature fields exist
					DMibExpectTrue(SignatureJson.f_GetMember("DigestAlgorithm") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("InputType") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Manifest") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Signature") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Certificate") != nullptr);

					// Verify with fg_VerifyFiles
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestFilePath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectTrue(VerifyResult);
				}

				// Setup MalterlibCloud trust to connect to CodeSigningManager
				{
					// Get MalterlibCloud's host ID
					CStr CloudClientExe = CloudClientDirectory + "/MalterlibCloud";
					auto CloudClientHostIDResult = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch{CloudClientExe + CFile::mc_ExecutableExtension, fGetParams({"--trust-host-id"}), CloudClientDirectory}
						)
					;
					CStr CloudClientHostID = CloudClientHostIDResult.f_GetStdOut().f_Trim();

					// Generate connection ticket from Signing CodeSigningManager
					auto Ticket = co_await SigningCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
						(
							CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{SigningCodeSigningManagerAddress}
						)
					;

					// Add connection to MalterlibCloud with CodeSigningManager namespace trusted
					co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientExe + CFile::mc_ExecutableExtension
								, fGetParams
								(
									{
										"--trust-connection-add"
										, "--trusted-namespaces"
										, _[CCodeSigningManager::mc_pDefaultNamespace].f_ToString(nullptr)
										, Ticket.m_Ticket.f_ToStringTicket()
									}
								)
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
					;

					// Add permissions on Signing CodeSigningManager for MalterlibCloud
					// MalterlibCloud needs permission to call the signing interface
					co_await SigningCodeSigningManagerLaunch.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissionsAdd(CloudClientHostID, TCMap<CStr, CPermissionRequirements>{{"CodeSigningManager/SignFiles", {}}})
						)
					;
				}

				{
					DMibTestPath("Sign File via MalterlibCloud CLI (stdout)");

					// Sign the test file via MalterlibCloud CLI, output to stdout
					auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams({"--code-signing-manager-sign-files", "--input", TestFilePath, "--authority", AuthorityName, "--signing-cert", SigningCertName, "--stdout"})
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap()
					;

					DMibAssertNoException(Result.f_Access());

					// Parse signature JSON from stdout
					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(Result->f_GetStdOut());

					DMibExpectTrue(SignatureJson.f_GetMember("DigestAlgorithm") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("InputType") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Manifest") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Signature") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Certificate") != nullptr);
				}

				{
					DMibTestPath("Sign File via MalterlibCloud CLI (default output)");

					// Sign the test file via MalterlibCloud CLI, output to default file
					CStr ExpectedOutputPath = TestFilePath + ".signature.json";

					// Clean up any existing signature file
					if (CFile::fs_FileExists(ExpectedOutputPath))
						CFile::fs_DeleteFile(ExpectedOutputPath);

					auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams({"--code-signing-manager-sign-files", "--input", TestFilePath, "--authority", AuthorityName, "--signing-cert", SigningCertName})
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap()
					;

					DMibAssertNoException(Result.f_Access());

					// Verify the signature file was created at the default path
					DMibExpectTrue(CFile::fs_FileExists(ExpectedOutputPath));

					// Parse and verify signature JSON from file
					CStr SignatureStr = CFile::fs_ReadStringFromFile(ExpectedOutputPath);
					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(SignatureStr);

					DMibExpectTrue(SignatureJson.f_GetMember("DigestAlgorithm") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("InputType") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Manifest") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Signature") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Certificate") != nullptr);

					// Verify the signature
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestFilePath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectTrue(VerifyResult);

					// Clean up
					CFile::fs_DeleteFile(ExpectedOutputPath);
				}

				{
					DMibTestPath("Sign File via MalterlibCloud CLI (custom output)");

					// Sign the test file via MalterlibCloud CLI, output to custom file
					CStr CustomOutputPath = RootDirectory + "/CustomSignature.json";

					// Clean up any existing signature file
					if (CFile::fs_FileExists(CustomOutputPath))
						CFile::fs_DeleteFile(CustomOutputPath);

					auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams
								(
									{
										"--code-signing-manager-sign-files"
										, "--input"
										, TestFilePath
										, "--authority"
										, AuthorityName
										, "--signing-cert"
										, SigningCertName
										, "--output"
										, CustomOutputPath
									}
								)
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap()
					;

					DMibAssertNoException(Result.f_Access());

					// Verify the signature file was created at the custom path
					DMibExpectTrue(CFile::fs_FileExists(CustomOutputPath));

					// Default path should NOT exist
					CStr DefaultOutputPath = TestFilePath + ".signature.json";
					DMibExpectFalse(CFile::fs_FileExists(DefaultOutputPath));

					// Parse and verify signature JSON from file
					CStr SignatureStr = CFile::fs_ReadStringFromFile(CustomOutputPath);
					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(SignatureStr);

					DMibExpectTrue(SignatureJson.f_GetMember("DigestAlgorithm") != nullptr);
					DMibExpectTrue(SignatureJson.f_GetMember("Signature") != nullptr);

					// Verify the signature
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestFilePath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectTrue(VerifyResult);
				}

				{
					DMibTestPath("Sign and Verify File");

					// Sign the test file
					auto SignResult = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams({"--code-signing-manager-sign-files", "--input", TestFilePath, "--authority", AuthorityName, "--signing-cert", SigningCertName, "--stdout"})
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap()
					;
					DMibAssertNoException(SignResult.f_Access());

					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(SignResult->f_GetStdOut());

					// Verify with fg_VerifyFiles
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestFilePath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectTrue(VerifyResult);
				}

				{
					DMibTestPath("Verify Fails with Modified File");

					// Sign the test file
					auto SignResult = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams({"--code-signing-manager-sign-files", "--input", TestFilePath, "--authority", AuthorityName, "--signing-cert", SigningCertName, "--stdout"})
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap();
					;
					DMibAssertNoException(SignResult.f_Access());
					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(SignResult->f_GetStdOut());

					// Modify the file
					CFile::fs_WriteStringToFile(TestFilePath, "Modified content");

					// Verify should fail
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestFilePath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectFalse(VerifyResult);

					// Restore the original file
					CFile::fs_WriteStringToFile(TestFilePath, "This is a test file for code signing.\nIt has multiple lines.\n");
				}

				{
					DMibTestPath("Sign Directory");

					// Create a test directory with files
					CStr TestDirPath = RootDirectory + "/TestSignDir";
					CFile::fs_CreateDirectory(TestDirPath);
					CFile::fs_WriteStringToFile(TestDirPath + "/File1.txt", "Content of file 1");
					CFile::fs_WriteStringToFile(TestDirPath + "/File2.txt", "Content of file 2");
					CFile::fs_CreateDirectory(TestDirPath + "/SubDir");
					CFile::fs_WriteStringToFile(TestDirPath + "/SubDir/File3.txt", "Content of file 3");

					// Sign the directory
					auto SignResult = co_await CProcessLaunchActor::fs_LaunchSimple
						(
							CProcessLaunchActor::CSimpleLaunch
							{
								CloudClientDirectory + "/MalterlibCloud" + CFile::mc_ExecutableExtension
								, fGetParams({"--code-signing-manager-sign-files", "--input", TestDirPath, "--authority", AuthorityName, "--signing-cert", SigningCertName, "--stdout"})
								, CloudClientDirectory
								, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
							}
						)
						.f_Wrap()
					;
					DMibAssertNoException(SignResult.f_Access());

					CEJsonSorted SignatureJson = CEJsonSorted::fs_FromString(SignResult->f_GetStdOut());
					DMibExpect(SignatureJson["InputType"].f_String(), ==, "Directory");

					// Verify
					TCVector<CByteVector> TrustedCAs;
					TrustedCAs.f_InsertLast(CACertificate);

					auto VerifyFunctor = fg_VerifyFiles(TestDirPath, SignatureJson, TrustedCAs);
					auto VerifyResult = co_await VerifyFunctor().f_Wrap();
					DMibExpectTrue(VerifyResult);
				}

				co_return {};
			};
		}
	};

	DMibTestRegister(CCodeSigningManager_Tests, Malterlib::Cloud);
}

