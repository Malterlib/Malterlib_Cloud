// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#if DMalterlibCloudCompatibilityTests

#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppLaunchHelper>
#include <Mib/Cloud/VersionManager>
#include <Mib/Cloud/AppManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/App/VersionManager>
#include <Mib/Cloud/App/AppManager>
#include <Mib/Process/Platform>
#include <Mib/Security/UniqueUserGroup>

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
using namespace NMib::NNetwork;
using namespace NMib::NEncoding;
using namespace NMib::NSecurity;
using namespace NMib::NException;
using namespace NMib::NFunction;
using namespace NMib::NTest;

#define DTestUpdateCompatibilityEnableLogging 0
#define DTestUpdateCompatibilityEnableOtherOutput 0

static fp64 g_Timeout = 60.0 * gc_TimeoutMultiplier;
static uint32 g_CompressionLevel = 1;

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
	TCMap<typename NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>::CType, typename NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>::CType> fg_CreateMap
		(
			tf_CKey && _First
			, tf_CParams && ...p_Params
		)
	{
		TCMap<typename NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>::CType, typename NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>::CType> Return;
		fg_CreateMapHelper<typename NTraits::TCRemoveReferenceAndQualifiers<tf_CKey>::CType, typename NTraits::TCRemoveReferenceAndQualifiers<tf_CValue>::CType>
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
	static auto constexpr mc_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;

	struct CPackageOptions
	{
		CPackageOptions(CStr const &_Package)
		{
			m_PackageOptions = EJSONType_Object;
			if (CFile::fs_FileExists(_Package + ".json"))
				m_PackageOptions = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(_Package + ".json"));

			auto FeatureFlags = m_PackageOptions.f_GetMemberValue("FeatureFlags", _[_]);
			for (auto &Flag : FeatureFlags.f_Array())
				m_FeatureFlags[Flag.f_String()];
		}

		bool f_HasFeatureFlag(CStr const &_Flag) const
		{
			return m_FeatureFlags.f_Exists(_Flag);
		}

		CEJSON m_PackageOptions;
		TCSet<CStr> m_FeatureFlags;
	};

	void fp_RunUpgradeTests(CStr const &_AppManagerPackage, CStr const &_VersionManagerPackage, CStr const &_KeyManagerPackage)
	{
		auto fPermissions = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, mc_WaitForSubscriptions};
			}
		;

		[[maybe_unused]] bool bCanDoEncription = false;
#ifdef DPlatformFamily_Linux
		bCanDoEncription = true;
#endif
		TCSharedPointer<CDefaultRunLoop> pRunLoop = fg_Construct();
		auto CleanupRunLoop = g_OnScopeExit / [&]
			{
				while (pRunLoop->m_RefCount.f_Get() > 0)
					pRunLoop->f_WaitOnceTimeout(0.1);
			}
		;
		TCActor<CDispatchingActor> HelperActor(fg_Construct(), pRunLoop->f_Dispatcher());
		auto CleanupHelperActor = g_OnScopeExit / [&]
			{
				HelperActor->f_BlockDestroy(pRunLoop->f_ActorDestroyLoop());
			}
		;
		CCurrentlyProcessingActorScope CurrentActor{HelperActor};

		auto FileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
		auto CleanupTestActor = g_OnScopeExit / [&]
			{
				FileActor->f_BlockDestroy(pRunLoop->f_ActorDestroyLoop());
			}
		;

		DMibExpectTrue(CFile::fs_FileExists(_AppManagerPackage));
		DMibExpectTrue(CFile::fs_FileExists(_VersionManagerPackage));
		DMibExpectTrue(CFile::fs_FileExists(_KeyManagerPackage));

		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();

		CStr RootDirectory = ProgramDirectory + "/CloudUpdateCompat";
		auto VersionManagerPermissionsForTest = fg_CreateMap<CStr, CPermissionRequirements>("Application/WriteAll", "Application/ReadAll", "Application/TagAll");

		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

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
				CProcessLaunch::fs_KillProcessesInDirectory("AppManager", {}, RootDirectory, 1000.0);
				CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 0.5);

				for (auto User : CreatedUsers)
				{
					CStr UID;
					if (NSys::fg_UserManagement_UserExists(User, UID))
					{
						NSys::fg_UserManagement_DeleteUser(User);

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
					}
				}
				for (auto Group : CreatedGroups)
				{
					CStr GID;
					if (NSys::fg_UserManagement_GroupExists(Group, GID))
						NSys::fg_UserManagement_DeleteGroup(Group);
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
				TrustManager->f_BlockDestroy(pRunLoop->f_ActorDestroyLoop());
			}
		;

		CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(pRunLoop, g_Timeout);
		CTrustedSubscriptionTestHelper Subscriptions{TrustManager, g_Timeout};

		CDistributedActorTrustManager_Address ServerAddress;

		ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
		TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(pRunLoop, g_Timeout);

		CDistributedApp_LaunchHelperDependencies Dependencies;
		Dependencies.m_Address = ServerAddress.m_URL;
		Dependencies.m_TrustManager = TrustManager;
		Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(pRunLoop, g_Timeout);

		NMib::NConcurrency::CDistributedActorSecurity Security;
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
		Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(pRunLoop, g_Timeout);

		TCActor<CDistributedApp_LaunchHelper> LaunchHelper
			= fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestUpdateCompatibilityEnableLogging || DTestUpdateCompatibilityEnableOtherOutput)
		;

		auto CleanupLaunchHelper = g_OnScopeExit / [&]
			{
				LaunchHelper->f_BlockDestroy(pRunLoop->f_ActorDestroyLoop());
			}
		;

		auto fSetupAppManager = [&](CStr const &_Directory)
			{
				TCActorResultVector<void> AppManagerLaunchesResults;
				(
					g_Dispatch(FileActor) / [=]
					{
						CFile::fs_CreateDirectory(_Directory);
						CProcessLaunch::fs_LaunchTool(BinaryDirectory / "bin/bsdtar", {"--no-same-owner", "-xf", _AppManagerPackage}, _Directory);
					}
				)
				.f_CallSync(pRunLoop, g_Timeout);
			}
		;

		auto fLaunchAppManager = [&](CStr const &_Name, CStr const &_Dir) -> CDistributedApp_LaunchInfo
			{
				TCVector<CStr> ExtraParams;
				if (AppManagerPackageOptions.f_HasFeatureFlag("NoDaemonRunStandalone"))
					ExtraParams.f_Insert("--daemon-run-debug");
				else
					ExtraParams.f_Insert("--daemon-run-standalone");

				if (!AppManagerPackageOptions.f_HasFeatureFlag("NoAutoUpdateDelay"))
					ExtraParams.f_Insert("--auto-update-delay=1.0"); // Make auto update faster
#if DTestUpdateCompatibilityEnableOtherOutput
				ExtraParams.f_Insert("--log-launches-to-stderr");
#endif
				return LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchWithParams, "AppManager_{}"_f << _Name, _Dir / "AppManager", fg_Move(ExtraParams)).f_CallSync(pRunLoop, g_Timeout);
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
				CStr AppDir = CFile::fs_GetPath(_Application);
				CStr SocketFile = AppDir / (CFile::fs_GetFileNoExt(_Application) + ".localsocket");
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
				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-generate-ticket")).f_Trim();
			}
		;

		auto fGetHostID = [&](CStr const &_Application) -> CStr
			{
				return CProcessLaunch::fs_LaunchTool(_Application, fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
			}
		;

		auto fSetupAppManagerTrust = [&](CDistributedApp_LaunchInfo &_LaunchInfo, CStr const &_Directory) -> CDistributedActorTrustManager_Address
			{
				CDistributedActorTrustManager_Address Address;
				Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << _Directory);
				_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(Address).f_CallSync(pRunLoop, g_Timeout);
				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
					(
						CAppManagerInterface::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(_LaunchInfo.m_HostID)
						, mc_WaitForSubscriptions
					).f_CallSync(pRunLoop, g_Timeout)
				;
				try
				{
					_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissions(TestHostID, fg_CreateMap<CStr, CPermissionRequirements>("AppManager/VersionAppAll", "AppManager/CommandAll", "AppManager/AppAll"))
						)
						.f_CallSync(pRunLoop, g_Timeout)
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
				CAppManager AppManager = {fLaunchAppManager(_Name, _Directory)};
				AppManager.m_Address = fSetupAppManagerTrust(AppManager.m_LaunchInfo, _Directory);
				AppManager.m_AppManager = Subscriptions.f_SubscribeFromHost<CAppManagerInterface>(AppManager.m_LaunchInfo.m_HostID);
				AppManager.m_RootPath = _Directory;

				return AppManager;
			}
		;

		auto fSleepKeyManager = [&]
			{
				if (KeyManagerPackageOptions.f_HasFeatureFlag("BuggySigTerm"))
					NSys::fg_Thread_Sleep(1.0);
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
			)
			{
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

				CProcessLaunch::fs_LaunchTool(_AppManager.m_RootPath / "AppManager", Params, _AppManager.m_RootPath);
				CProcessLaunch::fs_LaunchTool
					(
					 	_AppManager.m_RootPath / "AppManager"
					 	, {"--application-change-settings", "--name", PackageName, "--version-manager-application", PackageName}
					 	, _AppManager.m_RootPath
					)
				;

				if (_PackageOptions.f_HasFeatureFlag("BuggySigTerm"))
					fSleepKeyManager();
			}
		;

		auto fGetAppInfo = [&](CAppManager const &_AppManager, CStr const &_Application)
			{
				NTime::CClock Clock;
				CStr LastError;

				while (Clock.f_GetTime() < g_Timeout)
				{
					try
					{
						return _AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_CallSync(pRunLoop, 2.0)[_Application];
					}
					catch (CException const &_Exception)
					{
						LastError = _Exception.f_GetErrorStr();
					}
				}

				DMibError("Timed out waiting for app info. Last error: {}", LastError);
			}
		;

		CStr KeyManagerPassword = fg_RandomID();
		CStr KeyManagerHostID;
		CStr KeyManagerExecutable = KeyManagerDir / "App/KeyManager/KeyManager";

		auto fSetupKeyManager = [&](CAppManager const &_AppManager)
			{
				fInstallAppManually(_AppManager, KeyManagerPackageOptions, _KeyManagerPackage, "KeyManager", "MalterlibCloudKeyManager", "TestTag");

				DMibAssert(fGetAppInfo(_AppManager, "KeyManager").m_Status, ==, "Launched");

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(KeyManagerExecutable, {"--provide-password"}, KeyManagerDir);

				TCSharedPointer<CStr> pStdErr = fg_Construct();
				TCSharedPointer<CStr> pStdOut = fg_Construct();
				TCActor<CProcessLaunchActor> LaunchActor = fg_Construct();

				SimpleLaunch.m_Params.m_fOnOutput = [LaunchActor, pStdErr, pStdOut, KeyManagerPassword](EProcessLaunchOutputType _OutputType, NMib::NStr::CStr const &_Output)
					{
						if (_OutputType == EProcessLaunchOutputType_StdErr)
						{
							*pStdErr += _Output;
							if (pStdErr->f_Find("Type password for key database: ") >= 0)
							{
								pStdErr->f_Clear();
								LaunchActor(&CProcessLaunchActor::f_SendStdIn, KeyManagerPassword + "\n\n\n\n") > fg_DiscardResult();
							}
						}
						else
						{
							*pStdOut += _Output;
							if (pStdOut->f_Find("Type password for key database: ") >= 0)
							{
								pStdOut->f_Clear();
								LaunchActor(&CProcessLaunchActor::f_SendStdIn, KeyManagerPassword + "\n\n\n\n") > fg_DiscardResult();
							}
						}
					}
				;
				SimpleLaunch.m_bWholeLineOutput = false;

				TCPromise<void> LaunchFinished;
				SimpleLaunch.m_Params.m_fOnStateChange = [LaunchFinished](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
					{
						if (_State.f_GetTypeID() == EProcessLaunchState_Exited)
						{
							if (_State.f_Get<EProcessLaunchState_Exited>() == 0)
								LaunchFinished.f_SetResult();
							else
								LaunchFinished.f_SetException(DMibErrorInstance("Error exit: {}"_f << _State.f_Get<EProcessLaunchState_Exited>()));
						}
						else if (_State.f_GetTypeID() == EProcessLaunchState_LaunchFailed)
							LaunchFinished.f_SetException(DMibErrorInstance(_State.f_Get<EProcessLaunchState_LaunchFailed>()));
					}
				;

				auto LaunchSubscription = LaunchActor(&CProcessLaunchActor::f_Launch, SimpleLaunch, HelperActor).f_CallSync(pRunLoop, g_Timeout);
				LaunchFinished.f_MoveFuture().f_CallSync(pRunLoop, g_Timeout);
				fAddListen(KeyManagerExecutable, true);

				KeyManagerHostID = fGetHostID(KeyManagerExecutable);

				fSleepKeyManager();
			}
		;

		auto fSetupAppManagerConnection = [&](CAppManager const &_AppManager, CStr const &_Executable, CStr const &_Namespace, TCVector<CStr> const &_Permissions, CStr const &_HostID)
			{
				for (auto &Permission : _Permissions)
					CProcessLaunch::fs_LaunchTool(_Executable, fg_CreateVector<CStr>("--trust-permission-add", "--host", _AppManager.m_LaunchInfo.m_HostID, Permission));

				CDistributedActorTrustManagerInterface::CChangeNamespaceHosts Hosts;
				Hosts.m_Hosts[_HostID];
				Hosts.m_Namespace = _Namespace;

				_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)(fg_Move(Hosts)).f_CallSync(pRunLoop, g_Timeout);

				auto Ticket = CDistributedActorTrustManagerInterface::CTrustTicket::fs_FromStringTicket(fGenerateTicket(_Executable));
				_AppManager.m_LaunchInfo.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(Ticket, g_Timeout, -1).f_CallSync(pRunLoop, g_Timeout);
			}
		;

		CStr VersionManagerExecutable = VersionManagerDir / "App/VersionManager/VersionManager";
		CStr VersionManagerHostID;
		TCDistributedActor<CVersionManager> VersionManager;

		auto fSetupVersionManager = [&](CAppManager const &_AppManager)
			{
				fInstallAppManually(_AppManager, VersionManagerPackageOptions, _VersionManagerPackage, "VersionManager", "MalterlibCloudVersionManager", "VersionManagerTestTag");

				DMibAssert(fGetAppInfo(_AppManager, "VersionManager").m_Status, ==, "Launched");

				fAddListen(VersionManagerExecutable);

				VersionManagerHostID = fGetHostID(VersionManagerExecutable);

				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
					(
						CVersionManager::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(VersionManagerHostID)
						, mc_WaitForSubscriptions
					)
					.f_CallSync(pRunLoop, g_Timeout)
				;

				TCVector<CStr> Permissions = {"Application/ListAll", "Application/ReadAll", "Application/TagAll", "Application/WriteAll"};

				for (auto &Permission : Permissions)
					CProcessLaunch::fs_LaunchTool(VersionManagerExecutable, fg_CreateVector<CStr>("--trust-permission-add", "--host", TestHostID, Permission));

				auto Ticket = CDistributedActorTrustManager::CTrustTicket::fs_FromStringTicket(fGenerateTicket(VersionManagerExecutable));
				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AddClientConnection)(Ticket, g_Timeout, -1).f_CallSync(pRunLoop, g_Timeout);

				VersionManager = Subscriptions.f_SubscribeFromHost<CVersionManager>(VersionManagerHostID);
			}
		;

		auto fUploadPackage = [&](CStr const &_Package, TCSet<CStr> const &_Tags)
			{
				auto PackageInfo = VersionManagerHelper.f_GetPackageInfo(_Package).f_CallSync(pRunLoop, g_Timeout);
				PackageInfo.m_VersionInfo.m_Tags = _Tags;
				CStr PackageName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_Package));

				VersionManagerHelper.f_Upload(VersionManager, PackageName, PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, _Package).f_CallSync(pRunLoop, g_Timeout);
				return PackageInfo;
			}
		;

		auto fWaitVersionsAvailable = [&](CAppManager const &_AppManager, CStr const &_Application)
			{
				NTime::CClock Timeout{true};
				while (_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_GetAvailableVersions)(_Application).f_CallSync(pRunLoop, g_Timeout).f_IsEmpty())
				{
					if (Timeout.f_GetTime() > g_Timeout)
						DMibError("Timed out waiting for version manager application to become available in AppManager");
					NSys::fg_Thread_Sleep(0.01f);
				}
			}
		;

		auto fSetupAppManagerSelfUpdate = [&](CAppManager const &_AppManager, CStr const &_Tag)
			{
				fWaitVersionsAvailable(_AppManager, "AppManager");
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings ApplicationSettings;
				ApplicationSettings.m_bAutoUpdate = true;
				ApplicationSettings.m_UpdateTags = TCSet<CStr>{_Tag};
				ApplicationSettings.m_UpdateBranches = TCSet<CStr>{"*"};
				ApplicationSettings.m_bSelfUpdateSource = true;
				ApplicationSettings.m_VersionManagerApplication = "AppManager";
				ApplicationSettings.m_ExecutableParameters = TCVector<CStr>{};

				_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_Add)("SelfUpdate", fg_Move(Add), fg_Move(ApplicationSettings)).f_CallSync(pRunLoop, g_Timeout);
			}
		;

		auto fAddAppManagerApp = [&](CAppManager const &_AppManager, CStr const &_Application, CStr const &_Executable, CStr const &_Tag)
			{
				fWaitVersionsAvailable(_AppManager, "AppManager");
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings ApplicationSettings;
				ApplicationSettings.m_bAutoUpdate = true;
				ApplicationSettings.m_UpdateTags = TCSet<CStr>{_Tag};
				ApplicationSettings.m_UpdateBranches = TCSet<CStr>{"*"};
				ApplicationSettings.m_VersionManagerApplication = _Application;
				if (_Executable)
					ApplicationSettings.m_Executable = _Executable;

				_AppManager.m_AppManager.f_CallActor(&CAppManagerInterface::f_Add)(_Application, fg_Move(Add), fg_Move(ApplicationSettings)).f_CallSync(pRunLoop, g_Timeout);
			}
		;

		TCMap<CStr, CVersionManagerHelper::CPackageInfo> AppPackageInfos;
		TCSet<CStr> DoneInitPackageInfo;

		auto fUpdateApp = [&](CStr const &_Name, TCSet<CStr> const &_Tags)
			{
				CStr AppArchive = "{}/TestApps/Dynamic/{}.tar"_f << ProgramDirectory << _Name;

				{
					CStr VersionInfoFile = "{}/TestApps/{}/{}VersionInfo.json"_f << ProgramDirectory << _Name << _Name;
					CEJSON VersionInfo = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile));
					CVersionManager::CVersionID VersionID;
					if (DoneInitPackageInfo(_Name).f_WasCreated())
						VersionID = AppPackageInfos[_Name].m_VersionID.m_VersionID;
					else
					{
						CStr Error;
						CVersionManager::fs_IsValidVersionIdentifier(VersionInfo.f_GetMemberValue("Version", "").f_String(), Error, &VersionID);
					}
					++VersionID.m_Revision;
					VersionInfo["Version"] = CStr::fs_ToStr(VersionID);
					CFile::fs_WriteStringToFile(VersionInfoFile, VersionInfo.f_ToString(), false);
				}

				auto PackageInfo = VersionManagerHelper.f_CreatePackage("{}/TestApps/{}"_f << ProgramDirectory << _Name, AppArchive, g_CompressionLevel).f_CallSync(pRunLoop, g_Timeout);

				PackageInfo.m_VersionInfo.m_Tags = _Tags;
				auto Flags = CVersionManager::CStartUploadVersion::EFlag_ForceOverwrite;
				VersionManagerHelper.f_Upload(VersionManager, _Name, PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, AppArchive, Flags).f_CallSync(pRunLoop, g_Timeout);
				return PackageInfo;
			}
		;

		auto fTagApp = [&](CStr const &_Name, CVersionManagerHelper::CPackageInfo const &_PackageInfo, TCSet<CStr> const &_Tags)
			{
				CVersionManager::CChangeTags ChangeTags;
				ChangeTags.m_AddTags = _Tags;
				ChangeTags.m_Application = _Name;
				ChangeTags.m_VersionID = _PackageInfo.m_VersionID.m_VersionID;
				VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags)).f_CallSync(pRunLoop, g_Timeout);
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
				NTime::CClock Timeout{true};
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
					NSys::fg_Thread_Sleep(0.01f);
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

					NSys::fg_Thread_Sleep(0.01f);
				}
			}
		;


		auto fResubscribeAppManager = [&](CAppManager &_AppManager)
			{
				_AppManager.m_AppManager = Subscriptions.f_SubscribeFromHost<CAppManagerInterface>(_AppManager.m_LaunchInfo.m_HostID);
			}
		;
		auto fResubscribeVersionManager = [&]()
			{
				VersionManager = Subscriptions.f_SubscribeFromHost<CVersionManager>(VersionManagerHostID);
			}
		;

		fSetupAppManager(KeyManagerDir);
		fSetupAppManager(VersionManagerDir);
		fSetupAppManager(AppManagerDir);

		auto AppManager_KeyManager = fInitAppManager("KeyManager", KeyManagerDir);
		auto AppManager_VersionManager = fInitAppManager("VersionManager", VersionManagerDir);
		auto AppManager_AppManager = fInitAppManager("AppManager", AppManagerDir);

		fSetupKeyManager(AppManager_KeyManager);
		DMibTestMark;
		fSetupAppManagerConnection(AppManager_VersionManager, KeyManagerExecutable, "com.malterlib/Cloud/KeyManager", {}, KeyManagerHostID);
		DMibTestMark;
		fSetupAppManagerConnection(AppManager_AppManager, KeyManagerExecutable, "com.malterlib/Cloud/KeyManager", {}, KeyManagerHostID);
		DMibTestMark;
		fSetupVersionManager(AppManager_VersionManager);
		DMibTestMark;

		fSetupAppManagerConnection(AppManager_KeyManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		DMibTestMark;
		fSetupAppManagerConnection(AppManager_VersionManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		DMibTestMark;
		fSetupAppManagerConnection(AppManager_AppManager, VersionManagerExecutable, "com.malterlib/Cloud/VersionManager", {"Application/ReadAll"}, VersionManagerHostID);
		DMibTestMark;

		auto AppManagerPackageInfo = fUploadPackage(_AppManagerPackage, {"TestTag", "VersionManagerTestTag", "AppManagerTestTag"});
		AppPackageInfos["AppManager"] = AppManagerPackageInfo;
		DMibTestMark;

		auto KeyManagerPackageInfo = fUploadPackage(_KeyManagerPackage, {"TestTag"});
		AppPackageInfos["KeyManager"] = KeyManagerPackageInfo;
		DMibTestMark;

		auto VersionManagerPackageInfo = fUploadPackage(_VersionManagerPackage, {"VersionManagerTestTag"});
		AppPackageInfos["VersionManager"] = VersionManagerPackageInfo;
		DMibTestMark;

		fSleepKeyManager();
		DMibTestMark;

		fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"}, KeyManagerPackageOptions.f_HasFeatureFlag("InvalidPackageTime"));
		DMibTestMark;

		fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
		DMibTestMark;

		fResubscribeVersionManager();
		DMibTestMark;

		fSetupAppManagerSelfUpdate(AppManager_KeyManager, "TestTag");
		DMibTestMark;
		fSetupAppManagerSelfUpdate(AppManager_AppManager, "TestTag");
		DMibTestMark;
		fSetupAppManagerSelfUpdate(AppManager_VersionManager, "VersionManagerTestTag");
		DMibTestMark;

		fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
		DMibTestMark;
		fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
		DMibTestMark;
		fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
		DMibTestMark;

		fAddAppManagerApp(AppManager_AppManager, "AppManager", "AppManager", "AppManagerTestTag");

		auto fUpdateApps = [&]()
			{
				KeyManagerPackageInfo = fUpdateApp("KeyManager", {"TestTag"});
				fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"});

				VersionManagerPackageInfo = fUpdateApp("VersionManager", {"VersionManagerTestTag"});
				fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});

				fResubscribeVersionManager();
				try
				{
					// Update version manager
					AppManagerPackageInfo = fUpdateApp("AppManager", {"VersionManagerTestTag"});
					fResubscribeAppManager(AppManager_VersionManager);
					fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
					fResubscribeVersionManager();

					// Update app manager manager
					fTagApp("AppManager", AppManagerPackageInfo, {"AppManagerTestTag"});
					fWaitForAppVersion(AppManager_AppManager, "AppManager", AppManagerPackageInfo, {"Launched", "No executable"});

					// Update rest
					fTagApp("AppManager", AppManagerPackageInfo, {"TestTag"});
					fResubscribeAppManager(AppManager_AppManager);
					fResubscribeAppManager(AppManager_KeyManager);
					fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
					fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
				}
				catch (CException const &_Exception)
				{
					if (!_Exception.f_GetErrorStr().f_StartsWith("Remote actor no longer exists"))
						throw;
				}
			}
		;

		auto fTagAllAppManager = [&]
			{
				fTagApp("AppManager", AppManagerPackageInfo, {"VersionManagerTestTag", "AppManagerTestTag", "TestTag"});
			}
		;
		auto fTagAllVersionManager = [&]
			{
				fTagApp("VersionManager", VersionManagerPackageInfo, {"VersionManagerTestTag", "TestTag"});
			}
		;
		auto fTagAllKeyManager = [&]
			{
				fTagApp("KeyManager", KeyManagerPackageInfo, {"TestTag"});
			}
		;

		auto fUpdateAppsSimultaneous = [&](TCVector<TCFunction<void ()>> &&_DoTags)
			{
				KeyManagerPackageInfo = fUpdateApp("KeyManager", {});
				VersionManagerPackageInfo = fUpdateApp("VersionManager", {});
				AppManagerPackageInfo = fUpdateApp("AppManager", {});

				for (auto &fTag : _DoTags)
					fTag();

				fResubscribeAppManager(AppManager_AppManager);
				fResubscribeAppManager(AppManager_KeyManager);
				fResubscribeAppManager(AppManager_VersionManager);

				fWaitForAppVersion(AppManager_AppManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
				fWaitForAppVersion(AppManager_KeyManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
				fWaitForAppVersion(AppManager_VersionManager, "SelfUpdate", AppManagerPackageInfo, {"Self update source - waiting for update"});
				fWaitForAppVersion(AppManager_KeyManager, "KeyManager", KeyManagerPackageInfo, {"Launched"});
				fWaitForAppVersion(AppManager_VersionManager, "VersionManager", VersionManagerPackageInfo, {"Launched"});
				fWaitForAppVersion(AppManager_AppManager, "AppManager", AppManagerPackageInfo, {"Launched", "No executable"});

				fResubscribeVersionManager();
			}
		;

		{
			DMibTestPath("Upgrade");
			fUpdateApps();
		}
		{
			DMibTestPath("UpgradeAgain");
			fUpdateApps();
		}
		{
			DMibTestPath("SimultaneousUpgrade1");
			fUpdateAppsSimultaneous({fTagAllKeyManager, fTagAllVersionManager, fTagAllAppManager});
		}
		{
			DMibTestPath("SimultaneousUpgrade2");
			fUpdateAppsSimultaneous({fTagAllKeyManager, fTagAllAppManager, fTagAllVersionManager});
		}
		{
			DMibTestPath("SimultaneousUpgrade3");
			fUpdateAppsSimultaneous({fTagAllVersionManager, fTagAllKeyManager, fTagAllAppManager});
		}
		{
			DMibTestPath("SimultaneousUpgrade4");
			fUpdateAppsSimultaneous({fTagAllVersionManager, fTagAllAppManager, fTagAllKeyManager});
		}
		{
			DMibTestPath("SimultaneousUpgrade5");
			fUpdateAppsSimultaneous({fTagAllAppManager, fTagAllKeyManager, fTagAllVersionManager});
		}
		{
			DMibTestPath("SimultaneousUpgrade6");
			fUpdateAppsSimultaneous({fTagAllAppManager, fTagAllVersionManager, fTagAllKeyManager});
		}
	}

public:

	void f_DoTests()
	{
		DMibTestCategory(NTest::CTestCategory("General") << NTest::CTestGroup("SuperUser"))
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();

			TCMap<CStr, TCVector<CStr>> Packages;

			CStr BinaryDirectory = ProgramDirectory / "TestApps/VersionManager";

			bool bDoneInit = false;

			TCSet<CStr> InitializedLatestPackages;

			auto fInit = [&](CStr const &_AppManager, CStr const &_VersionManager, CStr const &_KeyManager)
				{
					if (!bDoneInit)
					{
						bDoneInit = true;
#ifdef DPlatformFamily_Windows
						AllocConsole();
						SetConsoleCtrlHandler
							(
								nullptr
								, true
							)
						;
#endif
#if DTestUpdateCompatibilityEnableLogging
						fg_GetSys()->f_AddStdErrLogger();
#endif
						CFile::fs_CreateDirectory("{}/TestApps/Latest"_f << ProgramDirectory);
						CFile::fs_CreateDirectory("{}/TestApps/Dynamic"_f << ProgramDirectory);
					}

					auto fInitPackage = [&](CStr const &_PackagePath)
						{
							CStr Version = CFile::fs_GetFile(CFile::fs_GetPath(_PackagePath));
							if (Version != "Latest")
								return;

							CStr AppName = CFile::fs_GetFileNoExt(CFile::fs_GetFileNoExt(_PackagePath));
							if (!InitializedLatestPackages(AppName).f_WasCreated())
								return;

							CStr VersionInfoFile = "{}/TestApps/{}/{}VersionInfo.json"_f << ProgramDirectory << AppName << AppName;
							CEJSON VersionInfo = CEJSON::fs_FromString(CFile::fs_ReadStringFromFile(VersionInfoFile));
							CVersionManager::CVersionID VersionID;
							CStr Error;
							CVersionManager::fs_IsValidVersionIdentifier(VersionInfo.f_GetMemberValue("Version", "").f_String(), Error, &VersionID);
							++VersionID.m_Revision;
							VersionInfo["Version"] = CStr::fs_ToStr(VersionID);
							CFile::fs_WriteStringToFile(VersionInfoFile, VersionInfo.f_ToString(), false);

							TCSharedPointer<CDefaultRunLoop> pRunLoop = fg_Construct();
							auto CleanupRunLoop = g_OnScopeExit / [&]
								{
									while (pRunLoop->m_RefCount.f_Get() > 0)
										pRunLoop->f_WaitOnceTimeout(0.1);
								}
							;
							TCActor<CDispatchingActor> HelperActor(fg_Construct(), pRunLoop->f_Dispatcher());
							auto CleanupHelperActor = g_OnScopeExit / [&]
								{
									HelperActor->f_BlockDestroy(pRunLoop->f_ActorDestroyLoop());
								}
							;
							CCurrentlyProcessingActorScope CurrentActor{HelperActor};

							CVersionManagerHelper VersionManagerHelper(BinaryDirectory);

							VersionManagerHelper.f_CreatePackage("{}/TestApps/{}"_f << ProgramDirectory << AppName, _PackagePath, g_CompressionLevel).f_CallSync(pRunLoop, g_Timeout);
						}
					;

					fInitPackage(_AppManager);
					fInitPackage(_VersionManager);
					fInitPackage(_KeyManager);
				}
			;

			for (auto &AppDir : CFile::fs_FindFiles(ProgramDirectory / "CloudTestBinaries/*", EFileAttrib_Directory))
			{
				CStr AppName = CFile::fs_GetFile(AppDir);
				for (auto &PackagePath : CFile::fs_FindFiles(AppDir / "*.tar.gz", EFileAttrib_File, true))
					Packages[AppName].f_Insert(PackagePath);
			}

			for (auto &AppName : {"KeyManager", "AppManager", "VersionManager"})
			{
				CStr AppArchive = "{}/TestApps/Latest/{}.tar.gz"_f << ProgramDirectory << AppName;
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

							fInit(AppManager, VersionManager, KeyManager);
							fp_RunUpgradeTests(AppManager, VersionManager, KeyManager);
						};
					}
				}
			}
		};
	}
};

DMibTestRegister(CUpdateCompatibility_Tests, Malterlib::Cloud);

#endif
