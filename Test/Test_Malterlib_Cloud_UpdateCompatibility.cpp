// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#if DMalterlibCloudCompatibilityTests

#include <Mib/Cloud/App/AppManager>
#include <Mib/Cloud/App/VersionManager>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Concurrency/ActorSequencerActor>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedAppLaunchHelper>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Concurrency/LogError>
#include <Mib/CommandLine/AnsiEncodingParse>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JsonShortcuts>
#include <Mib/Process/Platform>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Security/UniqueUserGroup>

#ifdef DPlatformFamily_Windows
#include <Windows.h>
#endif

using namespace NMib;
using namespace NMib::NAtomic;
using namespace NMib::NCloud;
using namespace NMib::NConcurrency;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NEncoding;
using namespace NMib::NException;
using namespace NMib::NFile;
using namespace NMib::NFunction;
using namespace NMib::NNetwork;
using namespace NMib::NProcess;
using namespace NMib::NSecurity;
using namespace NMib::NStorage;
using namespace NMib::NStr;
using namespace NMib::NTest;
using namespace NMib::NTime;

#define DTestUpdateCompatibilityEnableOtherOutput 0

static fp64 g_Timeout = 2_minutes * gc_TimeoutMultiplier;
static uint32 g_CompressionLevel = 4;

static constexpr ch8 const *gc_AccessDeniedErrorMessage =
	"Failed to download application from version manager: Failed to download the application from any manager: Failed to start download on remote server: Access denied"
;

namespace
{
	template <typename tf_CKey, typename tf_CValue, typename... tf_CParams>
	void fg_CreateMapHelper(TCMap<tf_CKey, tf_CValue> &_Return)
	{
	}

	template <typename tf_CKey, typename tf_CValue, typename... tf_CParams>
	void fg_CreateMapHelper(TCMap<tf_CKey, tf_CValue> &_Return, tf_CKey &&_First, tf_CParams && ...p_Params)
	{
		_Return[fg_Forward<tf_CKey>(_First)];
		fg_CreateMapHelper<tf_CKey, tf_CValue>(_Return, fg_Forward<tf_CParams>(p_Params)...);
	}

	template <typename tf_CKey, typename tf_CValue, typename... tf_CParams>
	TCMap<NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>, NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>> fg_CreateMap
		(
			tf_CKey && _First
			, tf_CParams && ...p_Params
		)
	{
		TCMap<NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>, NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>> Return;
		fg_CreateMapHelper<NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>, NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>>
			(
				Return
				, fg_Forward<tf_CKey>(_First)
				, fg_Forward<tf_CParams>(p_Params)...
			)
		;
		return Return;
	}

	template <typename tf_CKey, typename tf_CValue, typename... tf_CParams>
	TCMap<tf_CKey, tf_CValue> fg_CreateMap(tf_CParams && ...p_Params)
	{
		TCMap<tf_CKey, tf_CValue> Return;
		fg_CreateMapHelper<tf_CKey, tf_CValue>(Return, fg_Forward<tf_CParams>(p_Params)...);
		return Return;
	}
}

class CUpdateCompatibility_Tests : public NMib::NTest::CTest
{
	static auto constexpr mcp_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;

	struct CPackageOptions
	{
		CPackageOptions(CStr const &_Package)
		{
			m_PackageOptions = EJsonType_Object;
			if (CFile::fs_FileExists(_Package + ".json"))
				m_PackageOptions = CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(_Package + ".json"));

			auto FeatureFlags = m_PackageOptions.f_GetMemberValue("FeatureFlags", _[]);
			for (auto &Flag : FeatureFlags.f_Array())
				m_FeatureFlags[Flag.f_String()];
		}

		bool f_HasFeatureFlag(CStr const &_Flag) const
		{
			return m_FeatureFlags.f_Exists(_Flag);
		}

		CEJsonSorted m_PackageOptions;
		TCSet<CStr> m_FeatureFlags;
	};

	struct CKeyManagerPasswordProvider : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;

		CKeyManagerPasswordProvider(CStr const &_RootPath, CStr const &_Password)
			: mp_RootPath(_RootPath)
			, mp_Password(_Password)
		{
			fp_Startup().f_DiscardResult();
		}

		TCFuture<void> f_WaitForProvide()
		{
			co_return co_await mp_WaitForProvide.f_Insert().f_Future();
		}

		TCFuture<void> f_ProvidePasswordIfNeeded()
		{
			auto LogPath = mp_RootPath / "Log/KeyManager.log";

			if (!CFile::fs_FileExists(LogPath))
				co_return {};

			CStr Contents;
			try
			{
				Contents = CFile::fs_ReadStringFromFile(LogPath, true);
			}
			catch (CExceptionFile const &)
			{
				co_return {};
			}

			auto Lines = Contents.f_Split("\n");
			for (auto &Line : Lines)
			{
				if (Line.f_Find("Waiting for user to provide password") < 0)
					continue;

				auto StrippedLine = NCommandLine::CAnsiEncodingParse::fs_StripEncoding(Line);

				CStr DateStr;
				CStr TimeStr;
				aint nParsed = 0;
				(CStr::CParse("{} {} ") >> DateStr >> TimeStr).f_Parse(StrippedLine, nParsed);
				if (nParsed != 2)
					continue;

				CStr DateTimeStr = "{} {}"_f << DateStr << TimeStr;
				CTime Time;

				if (!fg_ParseFullTimeStr(Time, DateTimeStr))
					continue;

				if (mp_LastLineTime.f_IsValid() && Time <= mp_LastLineTime)
					continue;

				mp_LastLineTime = Time;

				auto Result = co_await fp_ProvidePassword().f_Wrap();

				if (!Result)
				{
					for (auto &Promise : mp_WaitForProvide)
						Promise.f_SetException(Result.f_GetException());
					DMibLogWithCategory(ProvidePassword, Warning, "Error providing password: {}", Result.f_GetExceptionStr());
				}
				else
				{
					for (auto &Promise : mp_WaitForProvide)
						Promise.f_SetResult();
				}

				mp_WaitForProvide.f_Clear();
			}

			co_return {};
		}

	private:

		TCFuture<void> fp_Destroy() override
		{
			co_await fg_Move(mp_Sequencer).f_Destroy().f_Wrap() > fg_LogError("Test", "Failed to destroy sequencer");

			if (mp_TimerSubscription)
				co_await fg_Exchange(mp_TimerSubscription, nullptr)->f_Destroy().f_Wrap() > fg_LogError("Test", "Failed to timer subscription");

			co_return {};
		}

		TCFuture<void> fp_ProvidePassword()
		{
			CProcessLaunchActor::CSimpleLaunch SimpleLaunch(mp_RootPath / "KeyManager", {"--provide-password"}, mp_RootPath);

			if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
			{
				SimpleLaunch.m_ToLog = CProcessLaunchActor::ELogFlag_Error | CProcessLaunchActor::ELogFlag_StdErr;
				SimpleLaunch.m_LogName = "ProvidePassword";
			}

			TCSharedPointer<CStr> pStdErr = fg_Construct();
			TCSharedPointer<CStr> pStdOut = fg_Construct();
			TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();
			TCSharedPointer<CProcessLaunchActor::CSimpleLaunchResult> pLaunchResult = fg_Construct();

			co_await mp_Sequencer.f_Sequence();

			SimpleLaunch.m_Params.m_fOnOutput = [LaunchActor, pStdErr, pStdOut, pLaunchResult, this](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
				{
					auto fCheckOutput = [&](auto &_Output)
						{
							if (_Output.f_Find("Type password for key database: ") >= 0)
							{
								_Output.f_Clear();
								LaunchActor(&CProcessLaunchActor::f_SendStdIn, mp_Password + "\n\n\n\n").f_DiscardResult();
							}
						}
					;

					pLaunchResult->m_Output.f_Insert(CProcessLaunchActor::COutput{.m_Type = _OutputType, .m_Output = _Output});

					if (_OutputType == EProcessLaunchOutputType_StdErr)
					{
						*pStdErr += _Output;
						fCheckOutput(*pStdErr);
					}
					else if (_OutputType == EProcessLaunchOutputType_StdOut)
					{
						*pStdOut += _Output;
						fCheckOutput(*pStdOut);
					}
				}
			;
			SimpleLaunch.m_bWholeLineOutput = false;

			TCPromise<void> LaunchFinished;
			SimpleLaunch.m_Params.m_fOnStateChange = [LaunchFinished, pLaunchResult](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
					{
						if (_State.f_Get<EProcessLaunchState_Exited>() == 0)
							LaunchFinished.f_SetResult();
						else
						{
							LaunchFinished.f_SetException
								(
									DMibErrorInstance("Error exit: {}\n{}"_f << _State.f_Get<EProcessLaunchState_Exited>() << pLaunchResult->f_GetCombinedOut().f_Trim())
								)
							;
						}
					}
					else if (_State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
						LaunchFinished.f_SetException(DMibErrorInstance(_State.f_Get<EProcessLaunchState_LaunchFailed>()));
				}
			;

			auto LaunchSubscription = co_await LaunchActor(&CProcessLaunchActor::f_Launch, SimpleLaunch, fg_ThisActor(this)).f_Timeout(g_Timeout, "Timed out");
			co_await LaunchFinished.f_MoveFuture().f_Timeout(g_Timeout, "Timed out");

			co_return {};
		}

		TCFuture<void> fp_Startup()
		{
			mp_TimerSubscription = co_await fg_RegisterTimer
				(
					50_ms
					, [=, this, LogPath = mp_RootPath / "Log/KeyManager.log"]() -> TCFuture<void>
					{
						co_await f_ProvidePasswordIfNeeded();

						co_return {};
					}
				)
			;

			co_return {};
		}

		CTime mp_LastLineTime;
		CStr mp_RootPath;
		CStr mp_Password;
		CActorSubscription mp_TimerSubscription;
		CSequencer mp_Sequencer{"KeyManagerPasswordProvider"};

		TCVector<TCPromise<void>> mp_WaitForProvide;
	};

	void fp_RunUpgradeTests(CStr const &_AppManagerPackage, CStr const &_VersionManagerPackage, CStr const &_KeyManagerPackage, CStr const &_UniqueName)
	{
		auto fPermissions = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, mcp_WaitForSubscriptions};
			}
		;

		[[maybe_unused]] bool bCanDoEncryption = false;
#ifdef DPlatformFamily_Linux
		bCanDoEncryption = false;
#endif
		CActorRunLoopTestHelper RunLoopHelper;

		DMibExpectTrue(CFile::fs_FileExists(_AppManagerPackage));
		DMibExpectTrue(CFile::fs_FileExists(_VersionManagerPackage));
		DMibExpectTrue(CFile::fs_FileExists(_KeyManagerPackage));

		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();

		CStr RootDirectory = ProgramDirectory / "CU" / _UniqueName;
		auto VersionManagerPermissionsForTest = fg_CreateMap<CStr, CPermissionRequirements>("Application/WriteAll", "Application/ReadAll", "Application/TagAll");

		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

		for (mint i = 0; i < 5; ++i)
		{
			try
			{
				if (CFile::fs_FileExists(RootDirectory))
					CFile::fs_DeleteDirectoryRecursive(RootDirectory);

				CFile::fs_CreateDirectory(RootDirectory);

				break;
			}
			catch (NFile::CExceptionFile const &)
			{
			}
		}

		TCActor<CDistributedAppLogForwarder> LogForwarder{fg_Construct(RootDirectory), "Log Forwarder Actor"};

		auto CleanupLogForwarder = g_OnScopeExit / [&]
			{
				LogForwarder->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		LogForwarder(&CDistributedAppLogForwarder::f_StartMonitoring).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		CStr KeyManagerDir = RootDirectory / "KeyManager";
		CStr VersionManagerDir = RootDirectory / "VersionManager";
		CStr AppManagerDir = RootDirectory / "AppManager";

		TCVector<CStr> CreatedUsers;
		TCVector<CStr> CreatedGroups;

		auto fAddUserGroup = [&](CStr const &_Directory, CStr const &_User)
			{
#ifdef DPlatformFamily_Windows
				CUniqueUserGroup UserGroup("C:/M", _Directory);
#else
				CUniqueUserGroup UserGroup("/M", _Directory);
#endif
				CreatedUsers.f_Insert(UserGroup.f_GetUser(_User));
				CreatedGroups.f_Insert(UserGroup.f_GetGroup(_User));
				CreatedUsers.f_Insert(_User);
				CreatedGroups.f_Insert(_User);
			}
		;

		fAddUserGroup(KeyManagerDir, "MalterlibCloudKeyManager");
		fAddUserGroup(VersionManagerDir, "MalterlibCloudVersionManager");

		auto fCleanup = [&]
			{
				CProcessLaunch::fs_KillProcessesInDirectory("AppManager", {}, RootDirectory, 10.0);
				CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

				for (auto User : CreatedUsers)
				{
					CStr UID;
					try
					{
						if (NSys::fg_UserManagement_UserExists(User, UID))
						{
							CProcessLaunch::fs_KillProcesses
								(
									[&](CProcessInfo const &_ProcessInfo)
									{
										return _ProcessInfo.m_RealUID == UID;
									}
									, EProcessInfoFlag_User
									, 0.5
								)
							;

							NSys::fg_UserManagement_DeleteUser(User);
						}
					}
					catch (...)
					{
					}
				}
				for (auto Group : CreatedGroups)
				{
					try
					{
						CStr GID;
						if (NSys::fg_UserManagement_GroupExists(Group, GID))
							NSys::fg_UserManagement_DeleteGroup(Group);
					}
					catch (...)
					{
					}
				}
			}
		;

		fCleanup();

		auto Cleanup = g_OnScopeExit / [&]
			{
				fCleanup();
			}
		;

		CPackageOptions AppManagerPackageOptions(_AppManagerPackage);
		CPackageOptions KeyManagerPackageOptions(_KeyManagerPackage);
		CPackageOptions VersionManagerPackageOptions(_VersionManagerPackage);

		CStr BinaryDirectory = ProgramDirectory / "TestApps/VersionManager";
		CVersionManagerHelper VersionManagerHelper(BinaryDirectory);

		CFile::fs_CreateDirectory(RootDirectory);

		CTrustManagerTestHelper TrustManagerState;
		TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
		auto CleanupTrustManager = g_OnScopeExit / [&]
			{
				TrustManager->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		CTrustedSubscriptionTestHelper Subscriptions{TrustManager, g_Timeout};

		CDistributedActorTrustManager_Address ServerAddress;

		ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
		TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		CDistributedApp_LaunchHelperDependencies Dependencies;
		Dependencies.m_Address = ServerAddress.m_URL;
		Dependencies.m_TrustManager = TrustManager;
		Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		NMib::NConcurrency::CDistributedActorSecurity Security;
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
		Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		TCActor<CDistributedApp_LaunchHelper> LaunchHelper
			= fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestUpdateCompatibilityEnableOtherOutput)
		;

		auto CleanupLaunchHelper = g_OnScopeExit / [&]
			{
				LaunchHelper->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		auto fSetupAppManager = [&](CStr const &_Directory) -> TCUnsafeFuture<void>
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager ({})", CFile::fs_GetFile(_Directory));

				auto BlockingActorCheckout = fg_BlockingActor();

				TCFutureVector<void> AppManagerLaunchesResults;
				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [=]
						{
							CFile::fs_CreateDirectory(_Directory);
							CProcessLaunch::fs_LaunchTool(BinaryDirectory / "bin/bsdtar", {"--no-same-owner", "--no-xattr", "-xf", _AppManagerPackage}, _Directory);
						}
					)
				;

				co_return {};
			}
		;

		auto fLaunchAppManager = [&](CStr const &_Name, CStr const &_Dir) -> CDistributedApp_LaunchInfo
			{
				DMibLogWithCategory(Test, Info, "Launch AppManager ({})", _Name);

				TCVector<CStr> ExtraParams;
				if (AppManagerPackageOptions.f_HasFeatureFlag("NoDaemonRunStandalone"))
					ExtraParams.f_Insert("--daemon-run-debug");
				else
					ExtraParams.f_Insert("--daemon-run-standalone");

				if (!AppManagerPackageOptions.f_HasFeatureFlag("NoAutoUpdateDelay"))
					ExtraParams.f_Insert("--auto-update-delay=0.001"); // Make auto update faster
#if DTestUpdateCompatibilityEnableOtherOutput
				ExtraParams.f_Insert("--log-launches-to-stderr");
#endif
				CSystemEnvironment Environment;
				Environment["MalterlibAppManagerAutoUpdateDelay"] = "0.001";

				return LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchWithParams, "AppManager_{}"_f << _Name, _Dir / "AppManager", fg_Move(ExtraParams), fg_Move(Environment))
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
			}
		;

		struct CAppManager
		{
			CDistributedApp_LaunchInfo m_LaunchInfo;
			CDistributedActorTrustManager_Address m_Address;
			TCDistributedActor<CAppManagerInterface> m_AppManager;
			CStr m_RootPath;
		};

		auto fAddListen = [&](CStr const &_Application, bool _bOldFormat = false) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Add Listen ({})", CFile::fs_GetFile(_Application));

				CStr AppDir = CFile::fs_GetPath(_Application);
				CStr SocketFile = AppDir / "test.ls";
				CStr URL;

				if (_bOldFormat)
					URL = "wss://[UNIX:{}]/"_f << SocketFile;
				else
					URL = "wss://[UNIX(666):{}]/"_f << SocketFile;

				CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-listen-add", URL));
				return URL;
			}
		;

		auto fGenerateTicket = [&](CStr const &_Application) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Generate Ticket ({})", CFile::fs_GetFile(_Application));

				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-generate-ticket")).f_Trim();
			}
		;

		auto fGetHostID = [&](CStr const &_Application) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Get Host ID ({})", CFile::fs_GetFile(_Application));

				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
			}
		;

		auto fSetupAppManagerTrust = [&](CDistributedApp_LaunchInfo &_LaunchInfo, CStr const &_Directory) -> CDistributedActorTrustManager_Address
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager Trust ({})", CFile::fs_GetFile(_Directory));

				CDistributedActorTrustManager_Address Address;
				Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << _Directory);
				_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(Address).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
					(
						CAppManagerInterface::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(_LaunchInfo.m_HostID)
						, mcp_WaitForSubscriptions
					).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
				try
				{
					_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissions(TestHostID, fg_CreateMap<CStr, CPermissionRequirements>("AppManager/VersionAppAll", "AppManager/CommandAll", "AppManager/AppAll"))
						)
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}
				catch (CException const &_Exception)
				{
					if (_Exception.f_GetErrorStr() != "Function does not exist on remote actor")
						throw;

				}

				return Address;
			}
		;

		auto fInitAppManager = [&](CStr const &_Name, CStr const &_Directory) -> CAppManager
			{
				DMibLogWithCategory(Test, Info, "Init AppManager ({})", _Name);

				CAppManager AppManager = {fLaunchAppManager(_Name, _Directory)};
				AppManager.m_Address = fSetupAppManagerTrust(AppManager.m_LaunchInfo, _Directory);
				{
					DMibLogWithCategory(Test, Info, "Subscribe from host ({})", _Name);
					AppManager.m_AppManager = Subscriptions.f_SubscribeFromHost<CAppManagerInterface>(RunLoopHelper, AppManager.m_LaunchInfo.m_HostID);
				}
				AppManager.m_RootPath = _Directory;

				return AppManager;
			}
		;

		auto fSleepKeyManager = [&]
			{
				if (KeyManagerPackageOptions.f_HasFeatureFlag("BuggySigTerm"))
					RunLoopHelper.m_pRunLoop->f_Sleep(1.0);
			}
		;

		auto fInstallAppManually = [&]
			(
				CAppManager const &_AppManager
				, CPackageOptions const &_PackageOptions
				, CStr const &_Package
				, CStr const &_Executable
				, CStr const &_User
				, CStr const &_Tag
				, bool _bDoEncryption
			)
			{
				DMibLogWithCategory(Test, Info, "Install App Manually ({}, {})", CFile::fs_GetFile(_AppManager.m_RootPath), CFile::fs_GetFile(_Package));

				CStr PackageName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_Package));
				TCVector<CStr> Params = {"--application-add", "--force-overwrite", "--from-file", _Package, "--name", PackageName};

				if (_Executable)
				{
					Params.f_Insert({"--executable", _Executable});

					if (_PackageOptions.f_HasFeatureFlag("NoDaemonRunStandalone"))
						Params.f_Insert({"--executable-parameters", "[\"--daemon-run-debug\"]"});
					else
						Params.f_Insert({"--executable-parameters", "[\"--daemon-run-standalone\"]"});
				}

				if (_User)
				{
					Params.f_Insert({"--run-as-user", _User});
					Params.f_Insert({"--run-as-group", _User});
				}

				if (AppManagerPackageOptions.f_HasFeatureFlag("OldAutoUpdate"))
					Params.f_Insert({"--auto-update-tags", "[\"{}\"]"_f << _Tag, "--auto-update-branches", "[\"*\"]"});
				else
					Params.f_Insert({"--auto-update", "--update-tags", "[\"{}\"]"_f << _Tag, "--update-branches", "[\"*\"]"});

				if (_bDoEncryption && bCanDoEncryption && !AppManagerPackageOptions.f_HasFeatureFlag("NoEncryption"))
				{
					CStr EncryptionFile = _AppManager.m_RootPath / (CFile::fs_GetFile(_Package) + ".enc");

					CFile File;
					File.f_Open(EncryptionFile, EFileOpen_Write);
					File.f_SetPosition(constant_uint64(2) * 1024 * 1024 * 1024 - 4096);
					uint8 Buffer[4096] = {0};
					File.f_Write(Buffer, 4096);

					Params.f_Insert({"--encryption-storage", EncryptionFile});
				}

				{
					DMibLogWithCategory(Test, Info, "Add");
					CProcessLaunch::fs_LaunchTool(_AppManager.m_RootPath / "AppManager", Params, _AppManager.m_RootPath);
				}
				{
					DMibLogWithCategory(Test, Info, "Change Settings");
					CProcessLaunch::fs_LaunchTool
						(
							_AppManager.m_RootPath / "AppManager"
							, {"--application-change-settings", "--name", PackageName, "--version-manager-application", PackageName}
							, _AppManager.m_RootPath
						)
					;
				}

				if (_PackageOptions.f_HasFeatureFlag("BuggySigTerm"))
					fSleepKeyManager();
			}
		;

		auto fGetAppInfo = [&](CAppManager const &_AppManager, CStr const &_Application)
			{
				CClock Clock{true};
				CStr LastError;

				while (Clock.f_GetTime() < g_Timeout)
				{
					try
					{
						return _AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_CallSync(RunLoopHelper.m_pRunLoop, 2.0)[_Application];
					}
					catch (CException const &_Exception)
					{
						LastError = _Exception.f_GetErrorStr();
					}
				}

				DMibError("Timed out waiting for app info. Last error: {}"_f << LastError);
			}
		;

		CStr KeyManagerPassword = fg_RandomID();
		CStr KeyManagerHostID;
		CStr KeyManagerExecutable = KeyManagerDir / "App/KeyManager/KeyManager";

		TCActor<CKeyManagerPasswordProvider> KeyManagerPasswordProvideActor{fg_Construct(KeyManagerDir / "App/KeyManager", KeyManagerPassword), "Password provider"};

		auto fProvideKeyManagerPasswordIfNeeded = [&]
			{
				KeyManagerPasswordProvideActor(&CKeyManagerPasswordProvider::f_ProvidePasswordIfNeeded).f_DiscardResult();
			}
		;

		auto fSetupKeyManager = [&](CAppManager const &_AppManager)
			{
				DMibLogWithCategory(Test, Info, "Setup KeyManager ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				TCFuture<void> PasswordProvidedFuture = KeyManagerPasswordProvideActor(&CKeyManagerPasswordProvider::f_WaitForProvide);

				fInstallAppManually(_AppManager, KeyManagerPackageOptions, _KeyManagerPackage, "KeyManager", "MalterlibCloudKeyManager", "TestTag", false);

				DMibAssert(fGetAppInfo(_AppManager, "KeyManager").m_Status, ==, "Launched");
				fProvideKeyManagerPasswordIfNeeded();
				{
					DMibLogWithCategory(Test, Info, "Wait For Password Provide");
					fg_Move(PasswordProvidedFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				}

				fAddListen(KeyManagerExecutable, true);

				KeyManagerHostID = fGetHostID(KeyManagerExecutable);

				fSleepKeyManager();
			}
		;

		auto fSetupAppManagerConnection = [&](CAppManager const &_AppManager, CStr const &_Executable, CStr const &_Namespace, TCVector<CStr> const &_Permissions, CStr const &_HostID)
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager Connection ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				for (auto &Permission : _Permissions)
				{
					DMibLogWithCategory(Test, Info, "Add permission '{}'", Permission);
					CProcessLaunch::fs_LaunchTool(_Executable, fg_CreateVector<CStr>("--trust-permission-add", "--host", _AppManager.m_LaunchInfo.m_HostID, Permission));
				}

				CDistributedActorTrustManagerInterface::CChangeNamespaceHosts Hosts;
				Hosts.m_Hosts[_HostID];
				Hosts.m_Namespace = _Namespace;

				{
					DMibLogWithCategory(Test, Info, "Allow Hosts");
					_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
						(fg_Move(Hosts)).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}

				{
					DMibLogWithCategory(Test, Info, "Connect");
					auto Ticket = CDistributedActorTrustManagerInterface::CTrustTicket::fs_FromStringTicket(fGenerateTicket(_Executable));
					{
						DMibLogWithCategory(Test, Info, "Add Client Connection");
						_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)
							(Ticket, g_Timeout, -1).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
						;
					}
				}
			}
		;

		CStr VersionManagerExecutable = VersionManagerDir / "App/VersionManager/VersionManager";
		CStr VersionManagerHostID;
		TCDistributedActor<CVersionManager> VersionManager;

		auto fSetupVersionManager = [&](CAppManager const &_AppManager)
			{
				DMibLogWithCategory(Test, Info, "Setup VersionManager ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				fInstallAppManually
					(
						_AppManager
						, VersionManagerPackageOptions
						, _VersionManagerPackage
						, "VersionManager"
						, "MalterlibCloudVersionManager"
						, "VersionManagerTestTag"
						, true
					)
				;

				DMibAssert(fGetAppInfo(_AppManager, "VersionManager").m_Status, ==, "Launched");

				fAddListen(VersionManagerExecutable);

				VersionManagerHostID = fGetHostID(VersionManagerExecutable);

				{
					DMibLogWithCategory(Test, Info, "Allow Hosts");

					TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
						(
							CVersionManager::mc_pDefaultNamespace
							, fg_CreateSet<CStr>(VersionManagerHostID)
							, mcp_WaitForSubscriptions
						)
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}

				TCVector<CStr> Permissions = {"Application/ListAll", "Application/ReadAll", "Application/TagAll", "Application/WriteAll"};

				for (auto &Permission : Permissions)
				{
					DMibLogWithCategory(Test, Info, "Add permission '{}'", Permission);
					CProcessLaunch::fs_LaunchTool(VersionManagerExecutable, fg_CreateVector<CStr>("--trust-permission-add", "--host", TestHostID, Permission));
				}

				{
					DMibLogWithCategory(Test, Info, "Connect");
					auto Ticket = CDistributedActorTrustManager::CTrustTicket::fs_FromStringTicket(fGenerateTicket(VersionManagerExecutable));
					{
						DMibLogWithCategory(Test, Info, "Add Client Connection");
						TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AddClientConnection)(Ticket, g_Timeout, -1).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					}
				}

				{
					DMibLogWithCategory(Test, Info, "Subscribe");
					VersionManager = Subscriptions.f_SubscribeFromHost<CVersionManager>(RunLoopHelper, VersionManagerHostID);
				}
			}
		;

		TCMap<CStr, TCVector<CVersionManager::CVersionIDAndPlatform>> UploadedPackages;

		auto fUploadPackage = [&](CStr _Package, TCSet<CStr> _Tags) -> TCUnsafeFuture<CVersionManagerHelper::CPackageInfo>
			{
				DMibLogWithCategory(Test, Info, "Upload Package ({}, {})", CFile::fs_GetFile(_Package), CFile::fs_GetFile(CFile::fs_GetPath(_Package)));

				auto PackageInfo = co_await VersionManagerHelper.f_GetPackageInfo(_Package);
				PackageInfo.m_VersionInfo.m_Tags = _Tags;
				CStr PackageName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_Package));

				UploadedPackages[PackageName].f_Insert(PackageInfo.m_VersionID);
				co_await VersionManagerHelper.f_Upload(VersionManager, PackageName, PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, _Package);

				co_return fg_Move(PackageInfo);
			}
		;

		auto fRemoveOldPackages = [&](CStr const &_Name)
			{
				auto *pPackage = UploadedPackages.f_FindEqual(_Name);
				if (!pPackage)
					DMibError("No such package: {}"_f << _Name);

				auto &PackageVersions = *pPackage;
				if (PackageVersions.f_GetLen() != 1)
					DMibError("Incorrect package versions length: {}"_f << PackageVersions);

				auto ToRemove = fg_Move(PackageVersions);

				CStr ApplicationRoot = VersionManagerDir / "App/VersionManager/Applications" / _Name;

				for (auto &Version : ToRemove)
				{
					CStr VersionFileName = "{}_{}.{}.{}"_f << Version.m_VersionID.m_Branch << Version.m_VersionID.m_Major << Version.m_VersionID.m_Minor << Version.m_VersionID.m_Revision;
					CFile::fs_DeleteDirectoryRecursive(ApplicationRoot / VersionFileName);
				}
			}
		;

		auto fWaitVersionsAvailable = [&](CAppManager const &_AppManager, CStr const &_Application)
			{
				DMibLogWithCategory(Test, Info, "Wait Version Available ({}, {})", CFile::fs_GetFile(_AppManager.m_RootPath), _Application);

				CClock Timeout{true};
				while (_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_GetAvailableVersions)(_Application).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout).f_IsEmpty())
				{
					if (Timeout.f_GetTime() > g_Timeout)
						DMibError("Timed out waiting for version manager application to become available in AppManager");
					RunLoopHelper.m_pRunLoop->f_Sleep(0.01f);
				}
			}
		;

		auto fSetupAppManagerSelfUpdate = [&](CAppManager const &_AppManager, CStr const &_Tag)
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager SelfUpdate ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				fWaitVersionsAvailable(_AppManager, "AppManager");
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings ApplicationSettings;
				ApplicationSettings.m_bAutoUpdate = true;
				ApplicationSettings.m_UpdateTags = TCSet<CStr>{_Tag};
				ApplicationSettings.m_UpdateBranches = TCSet<CStr>{"*"};
				ApplicationSettings.m_bSelfUpdateSource = true;
				ApplicationSettings.m_VersionManagerApplication = "AppManager";
				ApplicationSettings.m_ExecutableParameters = TCVector<CStr>{};

				{
					DMibLogWithCategory(Test, Info, "Add");
					_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_Add)("SelfUpdate", fg_Move(Add), fg_Move(ApplicationSettings))
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}
			}
		;

		auto fAddAppManagerApp = [&](CAppManager const &_AppManager, CStr const &_Application, CStr const &_Executable, CStr const &_Tag)
			{
				DMibLogWithCategory(Test, Info, "Add AppManager App ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				fWaitVersionsAvailable(_AppManager, "AppManager");
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings ApplicationSettings;
				ApplicationSettings.m_bAutoUpdate = true;
				ApplicationSettings.m_UpdateTags = TCSet<CStr>{_Tag};
				ApplicationSettings.m_UpdateBranches = TCSet<CStr>{"*"};
				ApplicationSettings.m_VersionManagerApplication = _Application;
				if (_Executable)
					ApplicationSettings.m_Executable = _Executable;

				{
					DMibLogWithCategory(Test, Info, "Add");
					_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_Add)(_Application, fg_Move(Add), fg_Move(ApplicationSettings))
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}
			}
		;

		TCMap<CStr, CVersionManagerHelper::CPackageInfo> AppPackageInfos;
		TCSet<CStr> DoneInitPackageInfo;

		auto fUpdateApp = [&](CStr _Name, TCSet<CStr> _Tags) -> TCUnsafeFuture<CVersionManagerHelper::CPackageInfo>
			{
				DMibLogWithCategory(Test, Info, "Update App ({})", _Name);

				CStr AppArchive = "{}/TestApps/Dynamic/{}/{}.tar.zst"_f << ProgramDirectory << _UniqueName << _Name;
				CStr SourceTempPath = "{}/TestApps/LatestSource/{}/{}"_f << ProgramDirectory << _UniqueName << _Name;

				{

					CStr VersionInfoFile = "{}/{}VersionInfo.json"_f << SourceTempPath << _Name;
					CEJsonSorted VersionInfo = CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile));
					CVersionManager::CVersionID VersionID;
					CStr Platform;
					if (DoneInitPackageInfo(_Name).f_WasCreated())
					{
						VersionID = AppPackageInfos[_Name].m_VersionID.m_VersionID;
						Platform = AppPackageInfos[_Name].m_VersionID.m_Platform;
					}
					else
					{
						CStr Error;
						CVersionManager::fs_IsValidVersionIdentifier(VersionInfo.f_GetMemberValue("Version", "").f_String(), Error, &VersionID);
						Platform = VersionInfo.f_GetMemberValue("Platform", "").f_String();
					}
					++VersionID.m_Revision;
					VersionInfo["Version"] = CStr::fs_ToStr(VersionID);
					VersionInfo["Platform"] = Platform;
					CFile::fs_WriteStringToFile(VersionInfoFile, VersionInfo.f_ToString(), false);
				}

				CFile::fs_CreateDirectory(CFile::fs_GetPath(AppArchive));

				auto PackageInfo = co_await VersionManagerHelper.f_CreatePackage(SourceTempPath, AppArchive, g_CompressionLevel);

				PackageInfo.m_VersionInfo.m_Tags = _Tags;
				auto Flags = CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite;
				co_await VersionManagerHelper.f_Upload(VersionManager, _Name, PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, AppArchive, Flags);

				UploadedPackages[_Name].f_Insert(PackageInfo.m_VersionID);

				co_return fg_Move(PackageInfo);
			}
		;

		auto fTagApp = [&](CStr _Name, CVersionManagerHelper::CPackageInfo _PackageInfo, TCSet<CStr> _Tags) -> TCUnsafeFuture<void>
			{
				for (mint i = 0; i < 3; ++i)
				{
					DMibLogWithCategory(Test, Info, "Tag App ({})", _Name);
					CVersionManager::CChangeTags ChangeTags;
					ChangeTags.m_AddTags = _Tags;
					ChangeTags.m_Application = _Name;
					ChangeTags.m_VersionID = _PackageInfo.m_VersionID.m_VersionID;

					auto Result = co_await VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags))
						.f_Timeout(g_Timeout, "Timed out tagging app")
						.f_Wrap()
					;

					if (Result)
						co_return {};

					if (i == 2)
						co_return Result.f_GetException(); // Throw the last exception

					DMibLogWithCategory(Test, Info, "Tag App ({}) failed, resubscribing...", _Name);
					VersionManager = co_await Subscriptions.f_SubscribeFromHostAsync<CVersionManager>(VersionManagerHostID);
				}
				co_return {};
			}
		;

		auto fWaitForAppVersion = [&]
			(
				CAppManager const &_AppManager
				, CStr const &_Application
				, CVersionManagerHelper::CPackageInfo const &_PackageInfo
				, TCVector<CStr> const &_ExpectedStatuses
				, bool _bCanHaveInvalidTime = false
			)
			{
				DMibLogWithCategory(Test, Info, "Wait For App ({}, {})", CFile::fs_GetFile(_AppManager.m_RootPath), _Application);

				CClock Timeout{true};
				CAppManagerInterface::CApplicationInfo AppInfo;
				auto fVersionIsEqual = [&]
					{
						AppInfo = fGetAppInfo(_AppManager, _Application);
						if (_bCanHaveInvalidTime)
							return AppInfo.m_Version == _PackageInfo.m_VersionID;
						else
							return AppInfo.m_Version == _PackageInfo.m_VersionID && AppInfo.m_VersionInfo.m_Time == _PackageInfo.m_VersionInfo.m_Time;
					}
				;
				while (!fVersionIsEqual())
				{
					if (Timeout.f_GetTime() > g_Timeout)
					{
						DMibError
							(
								"Timed out waiting for application {} at {} to become updated. {} ({}) != {} ({}). Status: {}"_f
								<< _Application
								<< _AppManager.m_RootPath
								<< AppInfo.m_Version
								<< AppInfo.m_VersionInfo.m_Time
								<< _PackageInfo.m_VersionID
								<< _PackageInfo.m_VersionInfo.m_Time
								<< AppInfo.m_Status
							)
						;
					}
					RunLoopHelper.m_pRunLoop->f_Sleep(0.01f);
				}

				Timeout.f_Start();
				while (true)
				{
					auto AppInfo = fGetAppInfo(_AppManager, _Application);
					if (_ExpectedStatuses.f_Contains(AppInfo.m_Status) >= 0)
						break;

					if (Timeout.f_GetTime() > g_Timeout)
					{
						DMibError
							(
								"Timed out waiting for application {} at {} to start after update. {} != {vs}"_f
								<< _Application
								<< _AppManager.m_RootPath
								<< AppInfo.m_Status
								<< _ExpectedStatuses
							)
						;
					}

					RunLoopHelper.m_pRunLoop->f_Sleep(0.01f);
				}
			}
		;


		auto fResubscribeAppManager = [&](CAppManager &_AppManager)
			{
				DMibLogWithCategory(Test, Info, "Resubscribe AppManager ({})", CFile::fs_GetFile(_AppManager.m_RootPath));
				_AppManager.m_AppManager = Subscriptions.f_SubscribeFromHost<CAppManagerInterface>(RunLoopHelper, _AppManager.m_LaunchInfo.m_HostID);
			}
		;
		auto fResubscribeVersionManager = [&]()
			{
				DMibLogWithCategory(Test, Info, "Resubscribe VersionManager");
				VersionManager = Subscriptions.f_SubscribeFromHost<CVersionManager>(RunLoopHelper, VersionManagerHostID);
			}
		;

		{
			DMibLogWithCategory(Test, Info, "Setup Root App Managers");
			auto KeyManagerFuture = fSetupAppManager(KeyManagerDir);
			auto VersionManagerFuture = fSetupAppManager(VersionManagerDir);
			auto AppManagerFuture = fSetupAppManager(AppManagerDir);
			fg_Move(KeyManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			fg_Move(VersionManagerFuture ).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			fg_Move(AppManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		}

		auto AppManager_KeyManager = fInitAppManager("KeyManager", KeyManagerDir);
		auto AppManager_VersionManager = fInitAppManager("VersionManager", VersionManagerDir);
		auto AppManager_AppManager = fInitAppManager("AppManager", AppManagerDir);

		fSetupKeyManager(AppManager_KeyManager);
		fSetupAppManagerConnection(AppManager_VersionManager, KeyManagerExecutable, "com.malterlib/Cloud/KeyManager", {}, KeyManagerHostID);
		fSetupAppManagerConnection(AppManager_AppManager, KeyManagerExecutable, "com.malterlib/Cloud/KeyManager", {}, KeyManagerHostID);
		fSetupVersionManager(AppManager_VersionManager);

		fSetupAppManagerConnection(AppManager_KeyManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		fSetupAppManagerConnection(AppManager_VersionManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		fSetupAppManagerConnection(AppManager_AppManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);

		bool bUpdatesOnFirstUpload = AppManagerPackageOptions.f_HasFeatureFlag("UpdatesOnFirstUpload");

		CVersionManagerHelper::CPackageInfo AppManagerPackageInfo;
		CVersionManagerHelper::CPackageInfo KeyManagerPackageInfo;
		CVersionManagerHelper::CPackageInfo VersionManagerPackageInfo;

		auto AppManagerPackageInfoFuture = fUploadPackage(_AppManagerPackage, {"TestTag", "VersionManagerTestTag", "AppManagerTestTag"});
		auto KeyManagerPackageInfoFuture = fUploadPackage(_KeyManagerPackage, {"TestTag"});
		if (bUpdatesOnFirstUpload)
		{
			AppManagerPackageInfo = fg_Move(AppManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			KeyManagerPackageInfo = fg_Move(KeyManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		}

		auto VersionManagerPackageInfoFuture = fUploadPackage(_VersionManagerPackage, {"VersionManagerTestTag"});

		if (!bUpdatesOnFirstUpload)
		{
			AppManagerPackageInfo = fg_Move(AppManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			KeyManagerPackageInfo = fg_Move(KeyManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		}

		VersionManagerPackageInfo = fg_Move(VersionManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		AppPackageInfos["AppManager"] = AppManagerPackageInfo;
		AppPackageInfos["KeyManager"] = KeyManagerPackageInfo;
		AppPackageInfos["VersionManager"] = VersionManagerPackageInfo;

		fSleepKeyManager();

		fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"}, KeyManagerPackageOptions.f_HasFeatureFlag("InvalidPackageTime"));
		fProvideKeyManagerPasswordIfNeeded();

		fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});

		if (bUpdatesOnFirstUpload)
			fResubscribeVersionManager();

		fSetupAppManagerSelfUpdate(AppManager_KeyManager, "TestTag");
		fSetupAppManagerSelfUpdate(AppManager_AppManager, "TestTag");
		fSetupAppManagerSelfUpdate(AppManager_VersionManager, "VersionManagerTestTag");

		fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
		fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
		fProvideKeyManagerPasswordIfNeeded();
		fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});

		fAddAppManagerApp(AppManager_AppManager, "AppManager", "AppManager", "AppManagerTestTag");

		auto fUpdateApps = [&]()
			{
				DMibLogWithCategory(Test, Info, "UpdateApps");

				fRemoveOldPackages("KeyManager");
				fRemoveOldPackages("VersionManager");
				fRemoveOldPackages("AppManager");
				{
					DMibLogWithCategory(Test, Info, "UpdateApps Update");
					auto KeyManagerPackageInfoFuture = fUpdateApp("KeyManager", {});
					auto VersionManagerPackageInfoFuture = fUpdateApp("VersionManager", {});
					auto AppManagerPackageInfoFuture = fUpdateApp("AppManager", {});
					AppManagerPackageInfo = fg_Move(AppManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					KeyManagerPackageInfo = fg_Move(KeyManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					VersionManagerPackageInfo = fg_Move(VersionManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				}

				{
					DMibLogWithCategory(Test, Info, "UpdateApps 0");
					fTagApp("KeyManager", KeyManagerPackageInfo, {"TestTag"}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"});
					fProvideKeyManagerPasswordIfNeeded();
				}

				{
					DMibLogWithCategory(Test, Info, "UpdateApps 1");
					fTagApp("VersionManager", VersionManagerPackageInfo, {"VersionManagerTestTag"}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
				}

				{
					DMibLogWithCategory(Test, Info, "UpdateApps 2");
					fResubscribeVersionManager();
				}
				{
					DMibLogWithCategory(Test, Info, "UpdateApps 3");
					fTagApp("AppManager", AppManagerPackageInfo, {"VersionManagerTestTag"}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fResubscribeAppManager(AppManager_VersionManager);
				}
				{
					DMibLogWithCategory(Test, Info, "UpdateApps 4");
					fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
					fResubscribeVersionManager();
				}

				// Update app manager manager
				{
					DMibLogWithCategory(Test, Info, "UpdateApps 5");
					fTagApp("AppManager", AppManagerPackageInfo, {"AppManagerTestTag"}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fWaitForAppVersion(AppManager_AppManager, "AppManager", AppManagerPackageInfo, {"Launched", "No exe", "No executable"});
				}

				// Update rest
				{
					DMibLogWithCategory(Test, Info, "UpdateApps 6");
					fTagApp("AppManager", AppManagerPackageInfo, {"TestTag"}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fResubscribeAppManager(AppManager_AppManager);
					fResubscribeAppManager(AppManager_KeyManager);
				}
				{
					DMibLogWithCategory(Test, Info, "UpdateApps 7");
					fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fProvideKeyManagerPasswordIfNeeded();
				}
			}
		;

		auto fTagAllAppManager = [&]
			{
				return fTagApp("AppManager", AppManagerPackageInfo, {"VersionManagerTestTag", "AppManagerTestTag", "TestTag"});
			}
		;
		auto fTagAllVersionManager = [&]
			{
				return fTagApp("VersionManager", VersionManagerPackageInfo, {"VersionManagerTestTag", "TestTag"});
			}
		;
		auto fTagAllKeyManager = [&]
			{
				return fTagApp("KeyManager", KeyManagerPackageInfo, {"TestTag"});
			}
		;

		auto fUpdateAppsSimultaneous = [&](TCVector<TCFunction<TCUnsafeFuture<void> ()>> &&_DoTags)
			{
				DMibLogWithCategory(Test, Info, "UpdateAppsSimultaneous");

				fRemoveOldPackages("KeyManager");
				fRemoveOldPackages("VersionManager");
				fRemoveOldPackages("AppManager");

				{
					DMibLogWithCategory(Test, Info, "UpdateAppsSimultaneous 0");
					auto KeyManagerPackageInfoFuture = fUpdateApp("KeyManager", {});
					auto VersionManagerPackageInfoFuture = fUpdateApp("VersionManager", {});
					auto AppManagerPackageInfoFuture = fUpdateApp("AppManager", {});
					AppManagerPackageInfo = fg_Move(AppManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					KeyManagerPackageInfo = fg_Move(KeyManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					VersionManagerPackageInfo = fg_Move(VersionManagerPackageInfoFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				}

				{
					DMibLogWithCategory(Test, Info, "UpdateAppsSimultaneous 1");
					TCFutureVector<void> TagFutures;
					for (auto &fTag : _DoTags)
						fTag() > TagFutures;
					fg_AllDone(TagFutures).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

					fResubscribeAppManager(AppManager_AppManager);
					fResubscribeAppManager(AppManager_KeyManager);
					fResubscribeAppManager(AppManager_VersionManager);
				}

				{
					DMibLogWithCategory(Test, Info, "UpdateAppsSimultaneous 2");
					fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Ready", "Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"});
					fProvideKeyManagerPasswordIfNeeded();
					fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
					fWaitForAppVersion(AppManager_AppManager, "AppManager", AppManagerPackageInfo, {"Launched", "No exe", "No executable"});

					fResubscribeVersionManager();
				}
			}
		;

		{
			DMibLogWithCategory(Test, Info, "Upgrade");
			fUpdateApps();
		}
		{
			DMibLogWithCategory(Test, Info, "UpgradeAgain");
			fUpdateApps();
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade1");
			fUpdateAppsSimultaneous({fTagAllKeyManager, fTagAllVersionManager, fTagAllAppManager});
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade2");
			fUpdateAppsSimultaneous({fTagAllKeyManager, fTagAllAppManager, fTagAllVersionManager});
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade3");
			fUpdateAppsSimultaneous({fTagAllVersionManager, fTagAllKeyManager, fTagAllAppManager});
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade4");
			fUpdateAppsSimultaneous({fTagAllVersionManager, fTagAllAppManager, fTagAllKeyManager});
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade5");
			fUpdateAppsSimultaneous({fTagAllAppManager, fTagAllKeyManager, fTagAllVersionManager});
		}
		{
			DMibLogWithCategory(Test, Info, "SimultaneousUpgrade6");
			fUpdateAppsSimultaneous({fTagAllAppManager, fTagAllVersionManager, fTagAllKeyManager});
		}
	}

	struct CAuthPasswordProvider : public CActor
	{
		using CActorHolder = CSeparateThreadActorHolder;

		CAuthPasswordProvider(CStr const &_Password)
			: mp_Password(_Password)
		{
		}

		void f_SetLaunchActor(TCActor<CProcessLaunchActor> _LaunchActor)
		{
			mp_LaunchActor = fg_Move(_LaunchActor);
		}

		void f_OnOutput(EProcessLaunchOutputType _OutputType, CStr const &_Output)
		{
			if (_OutputType == EProcessLaunchOutputType_StdOut || _OutputType == EProcessLaunchOutputType_StdErr)
			{
				mp_Buffer += _Output;

				// Check for password prompts
				if (mp_Buffer.f_Find("password:") >= 0 || mp_Buffer.f_Find("Password:") >= 0)
				{
					mp_Buffer.f_Clear();
					if (mp_LaunchActor)
						mp_LaunchActor(&CProcessLaunchActor::f_SendStdIn, mp_Password + "\n").f_DiscardResult();
				}
			}
		}

	private:
		CStr mp_Password;
		CStr mp_Buffer;
		TCActor<CProcessLaunchActor> mp_LaunchActor;
	};

	void fp_RunMultiAuthCompatibilityTests(CStr const &_AppManagerPackage, CStr const &_VersionManagerPackage, CStr const &_UniqueName)
	{
		auto fPermissions = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, mcp_WaitForSubscriptions};
			}
		;

		CActorRunLoopTestHelper RunLoopHelper;

		DMibExpectTrue(CFile::fs_FileExists(_AppManagerPackage));
		DMibExpectTrue(CFile::fs_FileExists(_VersionManagerPackage));
		// Note: KeyManager is not used in multi-auth tests

		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();

		CStr RootDirectory = ProgramDirectory / "CU-MA" / _UniqueName;

		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

		for (mint i = 0; i < 5; ++i)
		{
			try
			{
				if (CFile::fs_FileExists(RootDirectory))
					CFile::fs_DeleteDirectoryRecursive(RootDirectory);

				CFile::fs_CreateDirectory(RootDirectory);

				break;
			}
			catch (NFile::CExceptionFile const &)
			{
			}
		}

		TCActor<CDistributedAppLogForwarder> LogForwarder{fg_Construct(RootDirectory), "Log Forwarder Actor"};

		auto CleanupLogForwarder = g_OnScopeExit / [&]
			{
				LogForwarder->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		LogForwarder(&CDistributedAppLogForwarder::f_StartMonitoring).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		CStr VersionManagerDir = RootDirectory / "VersionManager";
		CStr AppManagerDir = RootDirectory / "AppManager";

		TCVector<CStr> CreatedUsers;
		TCVector<CStr> CreatedGroups;

		auto fAddUserGroup = [&](CStr const &_Directory, CStr const &_User)
			{
#ifdef DPlatformFamily_Windows
				CUniqueUserGroup UserGroup("C:/M", _Directory);
#else
				CUniqueUserGroup UserGroup("/M", _Directory);
#endif
				CreatedUsers.f_Insert(UserGroup.f_GetUser(_User));
				CreatedGroups.f_Insert(UserGroup.f_GetGroup(_User));
				CreatedUsers.f_Insert(_User);
				CreatedGroups.f_Insert(_User);
			}
		;

		fAddUserGroup(VersionManagerDir, "MalterlibCloudVersionManager");

		auto fCleanup = [&]
			{
				CProcessLaunch::fs_KillProcessesInDirectory("AppManager", {}, RootDirectory, 10.0);
				CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

				for (auto User : CreatedUsers)
				{
					CStr UID;
					try
					{
						if (NSys::fg_UserManagement_UserExists(User, UID))
						{
							CProcessLaunch::fs_KillProcesses
								(
									[&](CProcessInfo const &_ProcessInfo)
									{
										return _ProcessInfo.m_RealUID == UID;
									}
									, EProcessInfoFlag_User
									, 0.5
								)
							;

							NSys::fg_UserManagement_DeleteUser(User);
						}
					}
					catch (...)
					{
					}
				}
				for (auto Group : CreatedGroups)
				{
					try
					{
						CStr GID;
						if (NSys::fg_UserManagement_GroupExists(Group, GID))
							NSys::fg_UserManagement_DeleteGroup(Group);
					}
					catch (...)
					{
					}
				}
			}
		;

		fCleanup();

		auto Cleanup = g_OnScopeExit / [&]
			{
				fCleanup();
			}
		;

		CPackageOptions AppManagerPackageOptions(_AppManagerPackage);
		CPackageOptions VersionManagerPackageOptions(_VersionManagerPackage);

		// Helper to detect multi-auth support
		auto fSupportsMultiAuth = [](CPackageOptions const &_Options)
			{
				return !_Options.f_HasFeatureFlag("NoMultipleAuthenticationHandlers");
			}
		;

		bool bAppManagerSupportsMultiAuth = fSupportsMultiAuth(AppManagerPackageOptions);
		bool bVersionManagerSupportsMultiAuth = fSupportsMultiAuth(VersionManagerPackageOptions);

		CStr BinaryDirectory = ProgramDirectory / "TestApps/VersionManager";
		CVersionManagerHelper VersionManagerHelper(BinaryDirectory);

		CFile::fs_CreateDirectory(RootDirectory);

		CTrustManagerTestHelper TrustManagerState;
		TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
		auto CleanupTrustManager = g_OnScopeExit / [&]
			{
				TrustManager->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		CTrustedSubscriptionTestHelper Subscriptions{TrustManager, g_Timeout};

		CDistributedActorTrustManager_Address ServerAddress;

		ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
		TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		CDistributedApp_LaunchHelperDependencies Dependencies;
		Dependencies.m_Address = ServerAddress.m_URL;
		Dependencies.m_TrustManager = TrustManager;
		Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		NMib::NConcurrency::CDistributedActorSecurity Security;
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
		Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		TCActor<CDistributedApp_LaunchHelper> LaunchHelper
			= fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestUpdateCompatibilityEnableOtherOutput)
		;

		auto CleanupLaunchHelper = g_OnScopeExit / [&]
			{
				LaunchHelper->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
			}
		;

		auto fSetupAppManager = [&](CStr const &_Directory) -> TCUnsafeFuture<void>
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager ({})", CFile::fs_GetFile(_Directory));

				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [=]
						{
							CFile::fs_CreateDirectory(_Directory);
							CProcessLaunch::fs_LaunchTool(BinaryDirectory / "bin/bsdtar", {"--no-same-owner", "--no-xattr", "-xf", _AppManagerPackage}, _Directory);
						}
					)
				;

				co_return {};
			}
		;

		DMibTestMark;

		auto fLaunchAppManager = [&](CStr const &_Name, CStr const &_Dir) -> CDistributedApp_LaunchInfo
			{
				DMibLogWithCategory(Test, Info, "Launch AppManager ({})", _Name);

				TCVector<CStr> ExtraParams;
				if (AppManagerPackageOptions.f_HasFeatureFlag("NoDaemonRunStandalone"))
					ExtraParams.f_Insert("--daemon-run-debug");
				else
					ExtraParams.f_Insert("--daemon-run-standalone");

				if (!AppManagerPackageOptions.f_HasFeatureFlag("NoAutoUpdateDelay"))
					ExtraParams.f_Insert("--auto-update-delay=0.001");
#if DTestUpdateCompatibilityEnableOtherOutput
				ExtraParams.f_Insert("--log-launches-to-stderr");
#endif
				CSystemEnvironment Environment;
				Environment["MalterlibAppManagerAutoUpdateDelay"] = "0.001";

				return LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchWithParams, "AppManager_{}"_f << _Name, _Dir / "AppManager", fg_Move(ExtraParams), fg_Move(Environment))
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
			}
		;

		struct CAppManager
		{
			CDistributedApp_LaunchInfo m_LaunchInfo;
			CDistributedActorTrustManager_Address m_Address;
			TCDistributedActor<CAppManagerInterface> m_AppManager;
			CStr m_RootPath;
		};

		auto fAddListen = [&](CStr const &_Application, bool _bOldFormat = false) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Add Listen ({})", CFile::fs_GetFile(_Application));

				CStr AppDir = CFile::fs_GetPath(_Application);
				CStr SocketFile = AppDir / "test.ls";
				CStr URL;

				if (_bOldFormat)
					URL = "wss://[UNIX:{}]/"_f << SocketFile;
				else
					URL = "wss://[UNIX(666):{}]/"_f << SocketFile;

				CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-listen-add", URL));
				return URL;
			}
		;

		auto fGenerateTicket = [&](CStr const &_Application) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Generate Ticket ({})", CFile::fs_GetFile(_Application));

				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-generate-ticket")).f_Trim();
			}
		;

		auto fGetHostID = [&](CStr const &_Application) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Get Host ID ({})", CFile::fs_GetFile(_Application));

				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
			}
		;

		auto fSetupAppManagerTrust = [&](CDistributedApp_LaunchInfo &_LaunchInfo, CStr const &_Directory) -> CDistributedActorTrustManager_Address
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager Trust ({})", CFile::fs_GetFile(_Directory));

				CDistributedActorTrustManager_Address Address;
				Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << _Directory);
				_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(Address).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
					(
						CAppManagerInterface::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(_LaunchInfo.m_HostID)
						, mcp_WaitForSubscriptions
					).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
				try
				{
					_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissions(TestHostID, fg_CreateMap<CStr, CPermissionRequirements>("AppManager/VersionAppAll", "AppManager/CommandAll", "AppManager/AppAll"))
						)
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}
				catch (CException const &_Exception)
				{
					if (_Exception.f_GetErrorStr() != "Function does not exist on remote actor")
						throw;
				}

				return Address;
			}
		;

		auto fInitAppManager = [&](CStr const &_Name, CStr const &_Directory) -> CAppManager
			{
				DMibLogWithCategory(Test, Info, "Init AppManager ({})", _Name);

				CAppManager AppManager = {fLaunchAppManager(_Name, _Directory)};
				AppManager.m_Address = fSetupAppManagerTrust(AppManager.m_LaunchInfo, _Directory);
				{
					DMibLogWithCategory(Test, Info, "Subscribe from host ({})", _Name);
					AppManager.m_AppManager = Subscriptions.f_SubscribeFromHost<CAppManagerInterface>(RunLoopHelper, AppManager.m_LaunchInfo.m_HostID);
				}
				AppManager.m_RootPath = _Directory;

				return AppManager;
			}
		;

		// Helper struct for process results with stdin support
		struct CProcessWithStdinResult
		{
			CStr m_StdOut;
			CStr m_StdErr;
			uint32 m_ExitCode = 0;
			bool m_bSuccess = false;
		};

		// Launch a process and provide stdin input (typically for password prompts)
		auto fLaunchWithStdIn = [](CActorRunLoopTestHelper &_RunLoopHelper, CStr _Executable, TCVector<CStr> _Params, CStr _WorkingDir, TCVector<CStr> _StdInLines) -> CProcessWithStdinResult
			{
				using namespace NMib::NStr;

				TCSharedPointer<CProcessWithStdinResult> pResult = fg_Construct();

				TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();
				TCPromise<void> LaunchFinished;

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(_Executable, _Params, _WorkingDir);
				SimpleLaunch.m_SimpleFlags = CProcessLaunchActor::ESimpleLaunchFlag_None;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
				{
					SimpleLaunch.m_ToLog = CProcessLaunchActor::ELogFlag_Error | CProcessLaunchActor::ELogFlag_StdErr | CProcessLaunchActor::ELogFlag_StdOut;
					SimpleLaunch.m_LogName = CFile::fs_GetFile(_Executable);
				}

				SimpleLaunch.m_Params.m_fOnOutput = [LaunchActor, pResult, StdInBuffer = CStr(), nStdInIndex = mint(0), _StdInLines](EProcessLaunchOutputType _OutputType, CStr const &_Output) mutable
					{
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
				SimpleLaunch.m_bWholeLineOutput = false;

				SimpleLaunch.m_Params.m_fOnStateChange = [LaunchFinished, pResult](CProcessLaunchStateChangeVariant const &_State, fp64)
					{
						if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
						{
							pResult->m_ExitCode = _State.f_Get<EProcessLaunchState_Exited>();
							pResult->m_bSuccess = (pResult->m_ExitCode == 0);
							LaunchFinished.f_SetResult();
						}
						else if (_State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
						{
							pResult->m_StdErr += _State.f_Get<EProcessLaunchState_LaunchFailed>();
							LaunchFinished.f_SetResult();
						}
					}
				;

				try
				{
					auto LaunchSubscription = LaunchActor(&CProcessLaunchActor::f_Launch, SimpleLaunch, _RunLoopHelper.m_HelperActor)
						.f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout);
					LaunchFinished.f_MoveFuture().f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout);
					fg_Move(LaunchActor).f_Destroy().f_CallSync(_RunLoopHelper.m_pRunLoop, g_Timeout);
				}
				catch (CException const &_Exception)
				{
					pResult->m_StdErr += _Exception.f_GetErrorStr();
					pResult->m_ExitCode = 1;
					pResult->m_bSuccess = false;
				}

				return fg_Move(*pResult);
			}
		;

		// Register a password authentication factor for a user
		auto fRegisterUserPassword = [&](CStr _Executable, CStr _UserID, CStr _Password) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Registering password factor for user {}", _UserID);

				// Password registration prompts for password twice (for confirmation)
				auto Result = fLaunchWithStdIn
					(
						RunLoopHelper
						, _Executable
						, {"--trust-user-add-authentication-factor", "--authentication-factor", "Password", _UserID}
						, CFile::fs_GetPath(_Executable)
						, {_Password, _Password}  // Enter password twice for confirmation
					)
				;

				if (!Result.m_bSuccess)
				{
					DMibLogWithCategory(Test, Error, "Failed to register password: {}", Result.m_StdErr.f_Trim());
					return {};
				}

				CStr FactorID = Result.m_StdOut.f_Trim();
				DMibLogWithCategory(Test, Info, "Registered password factor: {}", FactorID);
				return FactorID;
			}
		;

		auto fSetupAppManagerConnection = [&](CAppManager const &_AppManager, CStr const &_Executable, CStr const &_Namespace, TCVector<CStr> const &_Permissions, CStr const &_HostID)
			{
				DMibLogWithCategory(Test, Info, "Setup AppManager Connection ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				for (auto &Permission : _Permissions)
				{
					DMibLogWithCategory(Test, Info, "Add permission '{}'", Permission);
					CProcessLaunch::fs_LaunchTool(_Executable, fg_CreateVector<CStr>("--trust-permission-add", "--host", _AppManager.m_LaunchInfo.m_HostID, Permission));
				}

				{
					CDistributedActorTrustManagerInterface::CChangeNamespaceHosts Hosts;
					Hosts.m_Hosts[_HostID];
					Hosts.m_Namespace = _Namespace;

					DMibLogWithCategory(Test, Info, "Allow Hosts for namespace '{}'", _Namespace);
					_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
						(fg_Move(Hosts)).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}

				{
					// Also allow the authentication namespace for password authentication
					CDistributedActorTrustManagerInterface::CChangeNamespaceHosts AuthHosts;
					AuthHosts.m_Hosts[_HostID];
					AuthHosts.m_Namespace = "com.malterlib/Concurrency/DistributedActorAuthentication";

					DMibLogWithCategory(Test, Info, "Allow Hosts for authentication namespace");
					_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
						(fg_Move(AuthHosts)).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}

				{
					DMibLogWithCategory(Test, Info, "Connect");
					auto Ticket = CDistributedActorTrustManagerInterface::CTrustTicket::fs_FromStringTicket(fGenerateTicket(_Executable));
					{
						DMibLogWithCategory(Test, Info, "Add Client Connection");
						_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)
							(Ticket, g_Timeout, -1).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
						;
					}
				}
			}
		;

		CStr VersionManagerExecutable = VersionManagerDir / "App/VersionManager/VersionManager";
		CStr VersionManagerHostID;
		TCDistributedActor<CVersionManager> VersionManager;

		auto fSetupVersionManager = [&](CAppManager const &_AppManager)
			{
				DMibLogWithCategory(Test, Info, "Setup VersionManager ({})", CFile::fs_GetFile(_AppManager.m_RootPath));

				// Install VersionManager manually
				CStr PackageName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_VersionManagerPackage));
				TCVector<CStr> Params = {"--application-add", "--force-overwrite", "--from-file", _VersionManagerPackage, "--name", PackageName};
				Params.f_Insert({"--executable", "VersionManager"});

				if (VersionManagerPackageOptions.f_HasFeatureFlag("NoDaemonRunStandalone"))
					Params.f_Insert({"--executable-parameters", "[\"--daemon-run-debug\"]"});
				else
					Params.f_Insert({"--executable-parameters", "[\"--daemon-run-standalone\"]"});

				Params.f_Insert({"--run-as-user", "MalterlibCloudVersionManager"});
				Params.f_Insert({"--run-as-group", "MalterlibCloudVersionManager"});

				if (AppManagerPackageOptions.f_HasFeatureFlag("OldAutoUpdate"))
					Params.f_Insert({"--auto-update-tags", "[\"VersionManagerTestTag\"]", "--auto-update-branches", "[\"*\"]"});
				else
					Params.f_Insert({"--auto-update", "--update-tags", "[\"VersionManagerTestTag\"]", "--update-branches", "[\"*\"]"});

				CProcessLaunch::fs_LaunchTool(_AppManager.m_RootPath / "AppManager", Params, _AppManager.m_RootPath);

				// Wait for launch
				CClock Timeout{true};
				while (Timeout.f_GetTime() < g_Timeout)
				{
					try
					{
						auto AppInfo = _AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_CallSync(RunLoopHelper.m_pRunLoop, 2.0)["VersionManager"];
						if (AppInfo.m_Status == "Launched")
							break;
					}
					catch (CException const &)
					{
					}
					RunLoopHelper.m_pRunLoop->f_Sleep(0.01f);
				}

				fAddListen(VersionManagerExecutable);

				VersionManagerHostID = fGetHostID(VersionManagerExecutable);

				{
					DMibLogWithCategory(Test, Info, "Allow Hosts");

					TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
						(
							CVersionManager::mc_pDefaultNamespace
							, fg_CreateSet<CStr>(VersionManagerHostID)
							, mcp_WaitForSubscriptions
						)
						.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
					;
				}

				// Grant non-Read permissions to host (no auth required)
				// NOTE: Application/ReadAll will be added per-user with auth factors in test scenarios
				TCVector<CStr> Permissions = {"Application/ListAll", "Application/TagAll", "Application/WriteAll"};

				for (auto &Permission : Permissions)
				{
					DMibLogWithCategory(Test, Info, "Add permission '{}'", Permission);
					CProcessLaunch::fs_LaunchTool(VersionManagerExecutable, fg_CreateVector<CStr>("--trust-permission-add", "--host", TestHostID, Permission));
				}

				{
					DMibLogWithCategory(Test, Info, "Connect");
					auto Ticket = CDistributedActorTrustManager::CTrustTicket::fs_FromStringTicket(fGenerateTicket(VersionManagerExecutable));
					{
						DMibLogWithCategory(Test, Info, "Add Client Connection");
						TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AddClientConnection)(Ticket, g_Timeout, -1).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					}
				}

				{
					DMibLogWithCategory(Test, Info, "Subscribe");
					VersionManager = Subscriptions.f_SubscribeFromHost<CVersionManager>(RunLoopHelper, VersionManagerHostID);
				}
			}
		;

		// Helper to add user-based permission with Password authentication factor
		auto fAddUserPermissionWithAuth = [&](CStr const &_HostID, CStr const &_UserID, CStr const &_Permission)
			{
				DMibLogWithCategory(Test, Info, "Add permission '{}' to user '{}' on host '{}' with Password auth", _Permission, _UserID, _HostID);

				CProcessLaunch::fs_LaunchTool
					(
						VersionManagerExecutable
						, fg_CreateVector<CStr>
							(
								"--trust-permission-add"
								, "--host", _HostID
								, "--user", _UserID
								, _Permission
								, "--authentication-factors", "[\"Password\"]"
								, "--max-lifetime", "0"  // No caching - always prompt for password
							)
					)
				;
			}
		;

		// Helper to add an application with password authentication
		// Returns empty string on success, error message on failure
		auto fApplicationAddWithAuth = [&](CAppManager const &_AppManager, CStr const &_AppName, CStr const &_UserID, CStr const &_Password) -> CStr
			{
				DMibLogWithCategory(Test, Info, "Adding app '{}' as user '{}' (requires auth)", _AppName, _UserID);

				auto Result = fLaunchWithStdIn
					(
						RunLoopHelper
						, _AppManager.m_RootPath / "AppManager"
						, fg_CreateVector<CStr>
							(
								"--application-add"
								, "--name", _AppName
								, "--update-tags", "[\"TestTag\"]"
								, "--auto-update"
								, "--authentication-user", _UserID
								, "AppManager"
							)
						, _AppManager.m_RootPath
						, {_Password}  // Provide password when prompted
					)
				;

				if (Result.m_bSuccess)
				{
					DMibLogWithCategory(Test, Info, "App '{}' added successfully", _AppName);
					return "";
				}
				else
				{
					DMibLogWithCategory(Test, Info, "App '{}' failed: {}", _AppName, Result.m_StdErr.f_Trim());
					return Result.m_StdErr.f_Trim();
				}
			}
		;

		// Result struct for fCreateAndExportUser
		struct CUserInfo
		{
			CStr m_UserID;
			CStr m_ExportedUser;
			CStr m_Password;
		};

		// Helper to create and export a user from AppManager with password authentication
		auto fCreateAndExportUser = [&](CAppManager const &_AppManager, CStr const &_UserName, CStr const &_Password) -> CUserInfo
			{
				DMibLogWithCategory(Test, Info, "Create and export user '{}' from {}", _UserName, CFile::fs_GetFile(_AppManager.m_RootPath));

				CStr AppManagerExe = _AppManager.m_RootPath / "AppManager";

				// Add user
				CStr UserID = CProcessLaunch::fs_LaunchTool(AppManagerExe, {"--trust-user-add", _UserName}).f_Trim();
				DMibLogWithCategory(Test, Info, "Created user '{}' with ID: {}", _UserName, UserID);

				// Register password authentication factor
				CStr FactorID = fRegisterUserPassword(AppManagerExe, UserID, _Password);
				DMibLogWithCategory(Test, Info, "Registered password factor: {}", FactorID);

				// Set as default user
				CProcessLaunch::fs_LaunchTool(AppManagerExe, {"--trust-user-set-default-user", UserID});

				// Export user with private data for import into other services
				CStr ExportedUser = CProcessLaunch::fs_LaunchTool(AppManagerExe, {"--trust-user-export", UserID, "--include-private-data"}).f_Trim();

				return {UserID, ExportedUser, _Password};
			}
		;

		// Helper to import user into VersionManager
		auto fImportUser = [&](CStr const &_ExportedUser)
			{
				DMibLogWithCategory(Test, Info, "Import user to VersionManager");
				CProcessLaunch::fs_LaunchTool(VersionManagerExecutable, {"--trust-user-import", _ExportedUser});
			}
		;

		// Setup infrastructure
		{
			DMibLogWithCategory(Test, Info, "Setup App Managers");
			auto VersionManagerFuture = fSetupAppManager(VersionManagerDir);
			auto AppManagerFuture = fSetupAppManager(AppManagerDir);
			fg_Move(VersionManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			fg_Move(AppManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		}

		auto AppManager_VersionManager = fInitAppManager("VersionManager", VersionManagerDir);
		auto AppManager_AppManager = fInitAppManager("AppManager", AppManagerDir);

		DMibTestMark;

		fSetupVersionManager(AppManager_VersionManager);

		DMibTestMark;

		fSetupAppManagerConnection(AppManager_VersionManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		// Note: For AppManager_AppManager, we don't grant Application/ReadAll here
		// Each test scenario will add the appropriate permissions (host-based for old clients, user-based for new)
		DMibTestMark;

		fSetupAppManagerConnection(AppManager_AppManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ListAll"}, VersionManagerHostID);

		// Upload test packages to VersionManager
		auto fUploadPackage = [&](CStr _Package, TCSet<CStr> _Tags) -> TCUnsafeFuture<CVersionManagerHelper::CPackageInfo>
			{
				DMibLogWithCategory(Test, Info, "Upload Package ({}, {})", CFile::fs_GetFile(_Package), CFile::fs_GetFile(CFile::fs_GetPath(_Package)));

				auto PackageInfo = co_await VersionManagerHelper.f_GetPackageInfo(_Package);
				PackageInfo.m_VersionInfo.m_Tags = _Tags;
				CStr PackageName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_Package));

				co_await VersionManagerHelper.f_Upload(VersionManager, PackageName, PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, _Package);

				co_return fg_Move(PackageInfo);
			}
		;

		// Upload packages for testing
		DMibTestMark;

		auto AppManagerPackageInfo = fg_Move(fUploadPackage(_AppManagerPackage, {"TestTag", "VersionManagerTestTag", "AppManagerTestTag"})).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		DMibTestMark;

		auto VersionManagerPackageInfo = fg_Move(fUploadPackage(_VersionManagerPackage, {"VersionManagerTestTag"})).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

		// Log version compatibility info
		DMibLogWithCategory(Test, Info, "Multi-Auth Compatibility Test:");
		DMibLogWithCategory(Test, Info, "  AppManager supports multi-auth: {}", bAppManagerSupportsMultiAuth);
		DMibLogWithCategory(Test, Info, "  VersionManager supports multi-auth: {}", bVersionManagerSupportsMultiAuth);

		DMibTestMark;

		// Scenarios A, B, D: At least one version doesn't support multi-auth
		// All use single sequential authentication
		DMibLogWithCategory(Test, Info, "=== Single Auth Test (at least one old version) ===");

		// Create and share user trust with password
		auto UserInfo = fCreateAndExportUser(AppManager_AppManager, "TestUser_SingleAuth", "PasswordSingle123");
		fImportUser(UserInfo.m_ExportedUser);

		// Add user-based permission with Password authentication
		fAddUserPermissionWithAuth(AppManager_AppManager.m_LaunchInfo.m_HostID, UserInfo.m_UserID, "Application/ReadAll");

		// Create user without permission for failure testing
		auto UserInfoNoPerms = fCreateAndExportUser(AppManager_AppManager, "TestUser_SingleAuth_NoPerms", "PasswordNoPerms123");
		fImportUser(UserInfoNoPerms.m_ExportedUser);
		// Note: Intentionally NOT calling fAddUserPermissionWithAuth

		DMibLogWithCategory(Test, Info, "Testing download with user authentication...");

		// Test that download works with password authentication
		CStr Error = fApplicationAddWithAuth(AppManager_AppManager, "TestApp_SingleAuth", UserInfo.m_UserID, UserInfo.m_Password);
		DMibExpect(Error, ==, "")("Download with authentication should succeed");
		DMibLogWithCategory(Test, Info, "Single auth test succeeded");

		// Test that download fails without permission
		DMibLogWithCategory(Test, Info, "Testing download without permission (should fail)...");
		CStr ErrorNoPerms = fApplicationAddWithAuth(AppManager_AppManager, "TestApp_SingleAuth_NoPerms", UserInfoNoPerms.m_UserID, UserInfoNoPerms.m_Password);
		DMibExpect(ErrorNoPerms, ==, gc_AccessDeniedErrorMessage)("User without permission should be denied even with correct password");
		DMibLogWithCategory(Test, Info, "Single auth failure test succeeded");

		// Test Scenario C: New -> New with parallel downloads (both support multi-auth)
		if (bVersionManagerSupportsMultiAuth)
		{
			// Test 2-4: Run parallel downloads in different permission modes
			enum class EParallelTestMode { ESucceed, EFail, EMixed };

			auto fParallelDownloadTest = [&](EParallelTestMode _Mode, CStr _TestName) -> TCUnsafeFuture<void>
				{
					DMibTestPath(_TestName);
					DMibLogWithCategory(Test, Info, "Testing parallel downloads ({})...", _TestName);

					TCVector<TCFuture<CStr>> Results;
					TCVector<CBlockingActorCheckout> Checkouts;
					TCVector<TCVector<CStr>> ExpectedErrors;

					for (mint i = 0; i < 5; ++i)
					{
						bool bShouldSucceed;
						switch (_Mode)
						{
							case EParallelTestMode::ESucceed: bShouldSucceed = true; break;
							case EParallelTestMode::EFail: bShouldSucceed = false; break;
							case EParallelTestMode::EMixed: bShouldSucceed = (i % 2 == 0); break; // alternating
						}

						auto const &User = bShouldSucceed ? UserInfo : UserInfoNoPerms;
						CStr AppName = "ParallelTestApp{}_{}"_f << _TestName << i;

						auto &Checkout = Checkouts.f_Insert(fg_BlockingActor());

						Results.f_Insert() =
							(
								g_Dispatch(Checkout) / [&, AppName = fg_Move(AppName), UserID = User.m_UserID, Password = User.m_Password]() mutable -> CStr
								{
									// Need our own run loop since we're on a different thread
									CActorRunLoopTestHelper LocalRunLoopHelper;
									auto Result = fLaunchWithStdIn
										(
											LocalRunLoopHelper
											, AppManager_AppManager.m_RootPath / "AppManager"
											, fg_CreateVector<CStr>
											(
												"--application-add"
												, "--name", AppName
												, "--update-tags", "[\"TestTag\"]"
												, "--auto-update"
												, "--authentication-user", UserID
												, "AppManager"
											)
											, AppManager_AppManager.m_RootPath
											, {Password}
										)
									;
									return Result.m_bSuccess ? "" : Result.m_StdErr.f_Trim();
								}
							).f_Call()
						;

						if (bAppManagerSupportsMultiAuth)
						{
							if (bShouldSucceed)
								ExpectedErrors.f_Insert({""});
							else
								ExpectedErrors.f_Insert({gc_AccessDeniedErrorMessage});
						}
						else
						{
							if (bShouldSucceed)
								ExpectedErrors.f_Insert({"Authentication already enabled", ""});
							else
								ExpectedErrors.f_Insert({"Authentication already enabled", gc_AccessDeniedErrorMessage});
						}
					}

					// Wait for all downloads and verify expected results
					for (mint i = 0; i < 5; ++i)
					{
						CStr AppName = "ParallelTestApp{}_{}"_f << _TestName << i;
						DMibTestPath(AppName);
						CStr Error = co_await fg_Move(Results[i]).f_Timeout(g_Timeout, "Parallel download timed out");
						DMibExpect(ExpectedErrors[i].f_Contains(Error), >=, 0)(Error);
					}

					DMibLogWithCategory(Test, Info, "Parallel downloads ({}) completed with expected results", _TestName);
					co_return {};
				}
			;

			DMibTestMark;
			fParallelDownloadTest(EParallelTestMode::ESucceed, "AllSucceed").f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			DMibTestMark;
			fParallelDownloadTest(EParallelTestMode::EFail, "AllFail").f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			DMibTestMark;
			fParallelDownloadTest(EParallelTestMode::EMixed, "Mixed").f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
		}

		DMibLogWithCategory(Test, Info, "Multi-Auth Compatibility Tests Complete");
	}

public:

	void f_DoTests()
	{
		DMibTestCategory(NTest::CTestCategory("General") << NTest::CTestGroup("SuperUser"))
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();

			TCMap<CStr, TCVector<CStr>> Packages;

			CStr BinaryDirectory = ProgramDirectory / "TestApps/VersionManager";

			auto fInit = [&](CStr &_AppManager, CStr &_VersionManager, CStr &_KeyManager, CStr const &_UniqueName)
				{
					auto fInitPackage = [&](CStr &o_PackagePath) -> TCUnsafeFuture<void>
						{
							if (!o_PackagePath)
								co_return {};

							CStr BasePath = "{}/TestApps/Latest/{}"_f << ProgramDirectory << _UniqueName;
							CFile::fs_CreateDirectory(BasePath);

							CStr AppName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(o_PackagePath));

							CStr SourceTempPath = "{}/TestApps/LatestSource/{}/{}"_f << ProgramDirectory << _UniqueName << AppName;

							CFile::fs_DiffCopyFileOrDirectory("{}/TestApps/{}"_f << ProgramDirectory << AppName, SourceTempPath, nullptr);

							CStr Version = CFile::fs_GetFile(CFile::fs_GetPath(o_PackagePath));
							if (Version != "Latest")
								co_return {};

							o_PackagePath = BasePath / o_PackagePath.f_RemovePrefix("Latest/");

							CStr VersionInfoFile = "{}/{}VersionInfo.json"_f << SourceTempPath << AppName;
							CEJsonSorted VersionInfo = CEJsonSorted::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile));
							CVersionManager::CVersionID VersionID;
							CStr Error;
							CVersionManager::fs_IsValidVersionIdentifier(VersionInfo.f_GetMemberValue("Version", "").f_String(), Error, &VersionID);
							++VersionID.m_Revision;
							VersionInfo["Version"] = CStr::fs_ToStr(VersionID);
							CFile::fs_WriteStringToFile(VersionInfoFile, VersionInfo.f_ToString(), false);

							CVersionManagerHelper VersionManagerHelper(BinaryDirectory);

							co_await VersionManagerHelper.f_CreatePackage(SourceTempPath, o_PackagePath, g_CompressionLevel);

							co_return {};
						}
					;

					CActorRunLoopTestHelper RunLoopHelper;

					auto AppManagerFuture = fInitPackage(_AppManager);
					auto VersionManagerFuture = fInitPackage(_VersionManager);
					auto KeyManagerFuture = fInitPackage(_KeyManager);

					fg_Move(AppManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fg_Move(VersionManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
					fg_Move(KeyManagerFuture).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
				}
			;

			for (auto &AppDir : CFile::fs_FindFiles(ProgramDirectory / "CloudTestBinaries/*", EFileAttrib_Directory))
			{
				CStr AppName = CFile::fs_GetFile(AppDir);
				for (auto &PackagePath : CFile::fs_FindFiles(AppDir / "*.tar.gz", EFileAttrib_File, true))
					Packages[AppName].f_Insert(PackagePath);
				for (auto &PackagePath : CFile::fs_FindFiles(AppDir / "*.tar.zst", EFileAttrib_File, true))
					Packages[AppName].f_Insert(PackagePath);
			}

			for (auto &AppName : {"KeyManager", "AppManager", "VersionManager"})
			{
				CStr AppArchive = "Latest/{}.tar.zst"_f << AppName;
				Packages[AppName].f_Insert(AppArchive);
			}

			auto &AppManagers = Packages["AppManager"];
			auto &VersionManagers = Packages["VersionManager"];
			auto &KeyManagers = Packages["KeyManager"];

			for (auto &AppManager : AppManagers)
			{
				for (auto &VersionManager : VersionManagers)
				{
					for (auto &KeyManager : KeyManagers)
					{
						CStr TestPath = "AppManager {}-VersionManager {}-KeyManager {}"_f
							<< CFile::fs_GetFile(CFile::fs_GetPath(AppManager))
							<< CFile::fs_GetFile(CFile::fs_GetPath(VersionManager))
							<< CFile::fs_GetFile(CFile::fs_GetPath(KeyManager))
						;

						DMibTestSuite(TestPath)
						{
							if (NProcess::NPlatform::fg_Process_GetElevation() == EProcessElevation_IsNotElevated)
								DMibError("You need to be elevated to run these tests (sudo)");

							CStr UniqueName = "{}-{}-{}"_f
								<< CFile::fs_GetFile(CFile::fs_GetPath(AppManager))
								<< CFile::fs_GetFile(CFile::fs_GetPath(VersionManager))
								<< CFile::fs_GetFile(CFile::fs_GetPath(KeyManager))
							;

							CStr AppManagerPath = AppManager;
							CStr VersionManagerPath = VersionManager;
							CStr KeyManagerPath = KeyManager;

							fInit(AppManagerPath, VersionManagerPath, KeyManagerPath, UniqueName);
							fp_RunUpgradeTests(AppManagerPath, VersionManagerPath, KeyManagerPath, UniqueName);
						};

					}

					CStr TestPath = "AppManager {}-VersionManager {}"_f
						<< CFile::fs_GetFile(CFile::fs_GetPath(AppManager))
						<< CFile::fs_GetFile(CFile::fs_GetPath(VersionManager))
					;
					DMibTestSuite("MultiAuth-" + TestPath)
					{
						if (NProcess::NPlatform::fg_Process_GetElevation() == EProcessElevation_IsNotElevated)
							DMibError("You need to be elevated to run these tests (sudo)");

						CStr UniqueName = "MA-{}-{}"_f
							<< CFile::fs_GetFile(CFile::fs_GetPath(AppManager))
							<< CFile::fs_GetFile(CFile::fs_GetPath(VersionManager))
						;

						CStr AppManagerPath = AppManager;
						CStr VersionManagerPath = VersionManager;
						CStr KeyManager;

						fInit(AppManagerPath, VersionManagerPath, KeyManager, UniqueName);
						fp_RunMultiAuthCompatibilityTests(AppManagerPath, VersionManagerPath, UniqueName);
					};
				}
			}
		};
	}
};

DMibTestRegister(CUpdateCompatibility_Tests, Malterlib::Cloud);

#endif
