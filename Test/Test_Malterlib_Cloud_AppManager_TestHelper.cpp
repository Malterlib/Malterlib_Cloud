// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"
#include <Mib/Cloud/App/TestApp>

#ifdef DPlatformFamily_Windows
#include "Windows.h"
#endif

namespace NMib::NCloud::NCloudManager
{
	extern uint64 g_MaxDatabaseSize;
}

namespace
{
	struct CInitReconnectDelay
	{
		CInitReconnectDelay()
		{
			// Override reconnect delay for whole process
			CDistributedAppActor_Settings::fs_GetGlobalDefaultSettings().m_ReconnectDelay = 1_ms;
		}
	};

	assure_used CInitReconnectDelay g_InitReconnectDelay;
}

namespace NMib::NCloud
{
	CStr const &CAppManagerTestHelper::CAppManagerInfo::f_GetHostID() const
	{
		return TCMap<CStr, CAppManagerInfo>::fs_GetKey(*this);
	}

	CAppManagerTestHelper::CState::CState(CStr const &_RootDirectory, EOption _Options, fp64 _Timeout)
		: m_RootDirectory(m_ProgramDirectory / _RootDirectory)
		, m_Options(_Options)
		, m_Timeout(_Timeout)
	{
		fg_TestAddCleanupPath(m_RootDirectory);

#ifdef DPlatformFamily_Windows
		AllocConsole();
		SetConsoleCtrlHandler
			(
				nullptr
				, true
			)
		;
#endif
	}

	CAppManagerTestHelper::CAppManagerTestHelper(CStr const &_RootDirectory, EOption _Options, fp64 _Timeout)
		: m_pState(fg_Construct(_RootDirectory, _Options, _Timeout))
	{
	}

	CAppManagerTestHelper::CState::~CState()
	{
		if (m_LaunchHelper)
			m_LaunchHelper->f_BlockDestroy();

		if (m_TrustManager)
			m_TrustManager->f_BlockDestroy();
	}

	CStr CAppManagerTestHelper::f_RootDirectory()
	{
		auto &State = *m_pState;

		return State.m_RootDirectory;
	}

	TCFuture<NStr::CStr> CAppManagerTestHelper::f_LaunchTool
		(
			NStr::CStr _Executable
			, NContainer::TCVector<NStr::CStr> _Params
			, NStr::CStr _WorkingDir
		)
	{
		auto &State = *m_pState;

		if (State.m_Options & EOption_EnableOtherOutput)
			_Params.f_Insert("--log-to-stderr");

		auto SimpleLaunch = CProcessLaunchActor::CSimpleLaunch(_Executable, _Params, _WorkingDir, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode);

		if (State.m_Options & EOption_EnableOtherOutput)
			SimpleLaunch.m_ToLog |= CProcessLaunchActor::ELogFlag_Error | CProcessLaunchActor::ELogFlag_StdErr | CProcessLaunchActor::ELogFlag_Info;

		co_return
			(
				co_await CProcessLaunchActor::fs_LaunchSimple(fg_Move(SimpleLaunch))
				.f_Timeout(State.m_Timeout, "Timed out waiting for tool launch: {} {vs}"_f << _Executable << _Params)
			)
			.f_GetStdOut()
		;
	}

	TCFuture<void> CAppManagerTestHelper::f_Destroy()
	{
		auto &State = *m_pState;
		TCFutureVector<void> Destroys;
		for (auto &Info : State.m_AppManagerInfos)
		{
			if (!Info.m_Launch)
				continue;

			Info.m_Launch->f_Destroy() > Destroys;
		}

		[[maybe_unused]] auto Result = co_await fg_AllDone(Destroys);

		if (State.m_LaunchHelper)
			co_await fg_Move(State.m_LaunchHelper).f_Destroy();

		if (State.m_TrustManager)
			co_await fg_Move(State.m_TrustManager).f_Destroy();


		if (State.m_LogForwarder)
			co_await fg_Move(State.m_LogForwarder).f_Destroy();

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_SetupTrust()
	{
		TCFutureVector<void> SetupTrustResults;

		auto &State = *m_pState;

		// Trust between app managers
		for (auto &AppManager : State.m_AppManagerInfos)
		{
			auto pAppManagerTrust = AppManager.m_pTrustInterface;
			auto &AppManagerTrust = *pAppManagerTrust;
			CStr AppManagerHostID = AppManager.f_GetHostID();
			auto TrustAppManagers = State.m_AppManagerHosts;
			TrustAppManagers.f_Remove(AppManagerHostID);
			if (!TrustAppManagers.f_IsEmpty())
			{
				AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						fs_NamespaceHosts("com.malterlib/Cloud/AppManagerCoordination", TrustAppManagers)
					)
					> SetupTrustResults
				;
			}

			State.m_TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
				(
					CAppManagerInterface::mc_pDefaultNamespace
					, TCSet<CStr>{AppManagerHostID}
					, mc_WaitForSubscriptions
				)
				> SetupTrustResults
			;

			for (auto &AppManagerInner : State.m_AppManagerInfos)
			{
				CStr AppManagerHostIDInner = AppManagerInner.f_GetHostID();
				if (AppManagerHostIDInner == AppManagerHostID)
					continue;

				auto pAppManagerTrustInner = AppManagerInner.m_pTrustInterface;

				TCPromise<void> Promise;
				AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{AppManager.m_Address}
					)
					> Promise / [=, this](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
					{
						auto &State = *m_pState;
						auto &AppManagerTrustInner = *pAppManagerTrustInner;
						AppManagerTrustInner.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, State.m_Timeout, -1) > Promise.f_ReceiveAny();
					}
				;
				Promise.f_MoveFuture() > SetupTrustResults;
			}
		}

		for (auto &AppManager : State.m_AppManagerInfos)
		{
			auto pAppManagerTrust = AppManager.m_pTrustInterface;
			auto &AppManagerTrust = *pAppManagerTrust;
			auto pCloudManagerTrust = State.m_CloudManagerLaunch->m_pTrustInterface;
			auto &CloudManagerTrust = *pCloudManagerTrust;
			CStr AppManagerHostID = AppManager.f_GetHostID();

			if (State.m_Options & EOption_EnableVersionManager)
			{
				auto pVersionManagerTrust = State.m_VersionManagerLaunch->m_pTrustInterface;
				auto &VersionManagerTrust = *pVersionManagerTrust;
				TCPromise<void> Promise;

				VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{State.m_VersionManagerServerAddress}
					)
					> Promise / [=, this, VersionManagerHostID = State.m_VersionManagerHostID]
					(
						CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_VersionManagerTicket
					)
					{
						auto &State = *m_pState;
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &VersionManagerTrust = *pVersionManagerTrust;
						AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_VersionManagerTicket.m_Ticket, State.m_Timeout, -1)
							+ VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fs_Permissions(AppManagerHostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll", {}}})
							)
							+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fs_NamespaceHosts(CVersionManager::mc_pDefaultNamespace, TCSet<CStr>{VersionManagerHostID})
							)
							> Promise / [=]()
							{
								Promise.f_SetResult();
							}
						;
					}
				;
				Promise.f_MoveFuture() > SetupTrustResults;
			}

			{
				TCPromise<void> Promise;

				CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{State.m_CloudManagerServerAddress}
					)
					+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(State.m_TestHostID, TCMap<CStr, CPermissionRequirements>{{"AppManager/VersionAppAll", {}}, {"AppManager/CommandAll", {}}, {"AppManager/AppAll", {}}})
					)
					+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions
						(
							State.m_CloudManagerHostID
							, TCMap<CStr, CPermissionRequirements>
							{
								{"AppManager/Command/ApplicationSubscribeChanges", {}}
								, {"AppManager/Command/ApplicationSubscribeUpdates", {}}
								, {"AppManager/AppAll", {}}
							}
						)
					)
					> Promise / [=, this, CloudManagerHostID = State.m_CloudManagerHostID]
					(
						CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_CloudManagerTicket
						, CVoidTag
						, CVoidTag
					)
					{
						auto &State = *m_pState;
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &CloudManagerTrust = *pCloudManagerTrust;
						AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_CloudManagerTicket.m_Ticket, State.m_Timeout, -1)
							+ CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fs_Permissions
								(
									AppManagerHostID
									, TCMap<CStr, CPermissionRequirements>
									{
										{"CloudManager/RegisterAppManager", {}}
										, {"CloudManager/ReportSensorReadings", {}}
										, {"CloudManager/ReportSensorReadingsOnBehalfOf/All", {}}
										, {"CloudManager/ReportLogEntries", {}}
										, {"CloudManager/ReportLogEntriesOnBehalfOf/All", {}}
									}
								)
							)
							+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fs_NamespaceHosts(CCloudManager::mc_pDefaultNamespace, TCSet<CStr>{CloudManagerHostID})
							)
							> Promise / [=]()
							{
								Promise.f_SetResult();
							}
						;
					}
				;
				Promise.f_MoveFuture() > SetupTrustResults;
			}
		}
		DMibTestMark;
		co_await fg_AllDone(SetupTrustResults).f_Timeout(State.m_Timeout, "Timed out waiting for trust setup");

		DMibTestMark;
		for (NTime::CClock Timer(true); true; co_await fg_Timeout(1.0))
		{
			bool bAllFinished = true;
			CStr VersionManagers;
			CStr CloudManagers;
			for (auto &AppManager : State.m_AppManagerInfos)
			{
				CStr ExecutableName = AppManager.m_RootDirectory / "AppManager";
	#ifdef DPlatformFamily_Windows
				ExecutableName += ".exe";
	#endif
				CStr VersionManagers = co_await f_LaunchTool(ExecutableName, {"--version-manager-list", "--no-color", "--table-type", "tab-separated"});
				CStr CloudManagers = co_await f_LaunchTool(ExecutableName, {"--cloud-manager-list", "--no-color", "--table-type", "tab-separated"});

				if (VersionManagers.f_SplitLine<true>().f_GetLen() < 1 || CloudManagers.f_SplitLine<true>().f_GetLen() < 1)
					bAllFinished = false;
			}

			if (bAllFinished)
				break;

			if (Timer.f_GetTime() > State.m_Timeout / 2)
				DMibError("Timed out waiting for version manager and cloud manager subscriptions:\nVersionManagers: {}\nCloudManagers: {}"_f << VersionManagers << CloudManagers);
		}
		DMibTestMark;

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_InstallTestApp(CStr _Name, CStr _Tag, CStr _Group, CStr _VersionManagerApplication)
	{
		auto &State = *m_pState;

		TCFutureVector<void> AddAppResults;
		for (auto &AppManager : State.m_AppManagerInfos)
		{
			if (State.m_Options & EOption_LaunchTestAppInApp)
			{
				if (_VersionManagerApplication == "TestApp")
				{
					CStr ExecutableName = AppManager.m_RootDirectory / "App" / _Name / "TestApp";
#ifdef DPlatformFamily_Windows
					ExecutableName += ".exe";
#endif
					State.m_InProcessLaunchScopes.f_Insert(fg_AppManager_RegisterInProcessFactory(ExecutableName, &fg_ConstructApp_TestApp));
				}
			}

			if (State.m_Options & EOption_EnableVersionManager)
			{
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings Settings;
				Settings.m_VersionManagerApplication = _VersionManagerApplication;
				Settings.m_bAutoUpdate = true;
				Settings.m_UpdateTags = TCSet<CStr>{_Tag};
				Settings.m_UpdateGroup = _Group;
				Settings.m_bLaunchInProcess = (State.m_Options & EOption_LaunchTestAppInApp) != EOption_None;
				Add.m_Version = State.m_PackageInfo.m_VersionID;

				AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_Add)(_Name, Add, Settings) > AddAppResults;
			}
			else
			{
				if (_VersionManagerApplication != "TestApp")
					DMibError("Only know about archive for TestApp");

				DMibTestMark;
				TCVector<CStr> Params =
					{
						"--application-add"
						, "--force-overwrite"
						, "--from-file"
						, State.m_TestAppArchive
						, "--name"
						, _Name
						, "--update-tags"
						, _Tag
						, "--update-group"
						, _Group
					}
				;

				if (State.m_Options & EOption_LaunchTestAppInApp)
					Params.f_Insert("--launch-in-process");

				TCPromise<void> LaunchPromise;
				f_LaunchTool(AppManager.m_RootDirectory / "AppManager", Params, AppManager.m_RootDirectory) > LaunchPromise.f_ReceiveAny();
				LaunchPromise.f_Future() > AddAppResults;
			}
		}
		DMibTestMark;
		co_await fg_AllDone(AddAppResults).f_Timeout(State.m_Timeout, "Timed out waiting for installing test apps");

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_CheckCloudManager(mint _Sequence)
	{
		auto &State = *m_pState;

		DMibTestPath("CloudManager{}"_f << _Sequence);

		auto AppManagers = co_await State.m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_Timeout(State.m_Timeout, "Timed out waiting for app manager enumeration 1");
		NTime::CClock Clock{true};
		while (AppManagers.f_GetLen() < State.m_nAppManagers && Clock.f_GetTime() < State.m_Timeout)
			AppManagers = co_await State.m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_Timeout(State.m_Timeout, "Timed out waiting for app manager enumeration 2");
		DMibExpect(AppManagers.f_GetLen(), ==, State.m_nAppManagers);

		NStr::CStr HostName = NProcess::NPlatform::fg_Process_GetFullyQualiedHostName();
		TCSet<CStr> ExpectedAppManagers;
		for (auto &Info : State.m_AppManagerInfos)
			ExpectedAppManagers[("{}/{}:{}"_f << Info.f_GetHostID() << HostName << (Info.m_RootDirectory)).f_GetStr()];

		TCSet<CStr> ActualAppManagers;
		for (auto &AppManager : AppManagers)
			ActualAppManagers[("{}/{}:{}"_f << AppManagers.fs_GetKey(AppManager) << AppManager.m_HostName << AppManager.m_ProgramDirectory).f_GetStr()];

		DMibExpect(ActualAppManagers, ==, ExpectedAppManagers);

		auto Applications = co_await State.m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_Timeout(State.m_Timeout, "Timed out waiting for app manager enumeration 3");

		auto fApplicationsDone = [&]()
			{
				if (Applications.f_GetLen() < State.m_nAppManagers) 
					return false;

				for (auto &Application : Applications)
				{
					if (Application.m_ApplicationInfo.m_Status != "Launched")
						return false;
				}

				return true;
			}
		;

		while (!fApplicationsDone() && Clock.f_GetTime() < State.m_Timeout)
		{
			co_await fg_Timeout(0.005);
			Applications = co_await State.m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_Timeout(State.m_Timeout, "Timed out waiting for app manager enumeration 4");
		}

		DMibExpect(Applications.f_GetLen(), ==, State.m_nAppManagers);

		for (auto &Application : Applications)
		{
			auto &ApplicationKey = Applications.fs_GetKey(Application);
			DMibExpect(ApplicationKey.m_Name, ==, "TestApp")(ETestFlag_Aggregated);
			DMibExpect(Application.m_ApplicationInfo.m_Status, ==, "Launched")(ETestFlag_Aggregated);
		}

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_StopCloudManager()
	{
		auto &State = *m_pState;

		if (State.m_CloudManagerLaunch)
			co_await State.m_CloudManagerLaunch->f_Destroy();
		State.m_CloudManagerLaunch.f_Clear();

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_StartCloudManager()
	{
		auto &State = *m_pState;

		State.m_CloudManagerLaunch = co_await State.m_LaunchHelper
			(
				&CDistributedApp_LaunchHelper::f_LaunchInProcess
				, "CloudManager"
				, State.m_CloudManagerDirectory
				, &fg_ConstructApp_CloudManager
				, NContainer::TCVector<NStr::CStr>{}
			)
			.f_Timeout(State.m_Timeout, "Timed out waiting for cloud manager launch")
		;

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_Setup(mint _nAppManagers)
	{
		auto &State = *m_pState;

		State.m_nAppManagers = _nAppManagers;
		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, State.m_RootDirectory, 10.0);

		for (mint i = 0; i < 5; ++i)
		{
			try
			{
				if (CFile::fs_FileExists(State.m_RootDirectory))
					CFile::fs_DeleteDirectoryRecursive(State.m_RootDirectory);
				break;
			}
			catch (NFile::CExceptionFile const &)
			{
			}
		}

		CFile::fs_CreateDirectory(State.m_RootDirectory);

		State.m_LogForwarder = fg_Construct(fg_Construct(State.m_RootDirectory), "Log Forwarder Actor");
		co_await State.m_LogForwarder(&CDistributedAppLogForwarder::f_StartMonitoring).f_Timeout(State.m_Timeout, "Timed out waiting for log forwarder to start");

		State.m_TrustManager = State.m_TrustManagerState.f_TrustManager("TestHelper");
		State.m_TestHostID = co_await State.m_TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_Timeout(State.m_Timeout, "Timed out waiting for host id of trust manager");
		State.m_Subscriptions = fg_Construct(State.m_TrustManager);

		State.m_ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << State.m_RootDirectory);
		co_await State.m_TrustManager(&CDistributedActorTrustManager::f_AddListen, State.m_ServerAddress).f_Timeout(State.m_Timeout, "Timed out waiting for listen addition");

		{
			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = State.m_ServerAddress.m_URL;
			Dependencies.m_TrustManager = State.m_TrustManager;
			Dependencies.m_DistributionManager = co_await State.m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager)
				.f_Timeout(State.m_Timeout, "Timed out waiting for distribution manager")
			;

			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CCloudManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CDistributedAppSensorReporter::mc_pDefaultNamespace);

			co_await Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_Timeout(State.m_Timeout, "Timed out waiting for set security");

			State.m_LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, (State.m_Options & EOption_EnableOtherOutput) != EOption_None);
		}
		if (State.m_Options & EOption_EnableVersionManager)
		{
			// Launch VersionManager
			State.m_VersionManagerDirectory = State.m_RootDirectory + "/VersionManager";
			CFile::fs_CreateDirectory(State.m_VersionManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(State.m_ProgramDirectory + "/TestApps/VersionManager", State.m_VersionManagerDirectory, nullptr);

			State.m_VersionManagerLaunch = co_await State.m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager"
					, State.m_VersionManagerDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for version manager launch")
			;

			DMibExpect(State.m_VersionManagerLaunch->m_HostID, !=, "");

		}
		{
			// Launch CloudManager
			State.m_CloudManagerDirectory = State.m_RootDirectory + "/CloudManager";
			CFile::fs_CreateDirectory(State.m_CloudManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(State.m_ProgramDirectory + "/TestApps/CloudManager", State.m_CloudManagerDirectory, nullptr);

			NCloudManager::g_MaxDatabaseSize = constant_uint64(1) * 1024 * 1024 * 1024; // Limit due to asan for example taking too much memory

			co_await f_StartCloudManager();

			DMibExpect(State.m_CloudManagerLaunch->m_HostID, !=, "");
		}
		{
			// Copy Cloud Client for debugging
			State.m_CloudClientDirectory = State.m_RootDirectory / "MalterlibCloud";
			CFile::fs_CreateDirectory(State.m_CloudClientDirectory);
			CFile::fs_DiffCopyFileOrDirectory(State.m_ProgramDirectory / "TestApps/MalterlibCloud", State.m_CloudClientDirectory, nullptr);
		}
		// Copy AppManagers to their directories
		{
			TCFutureVector<void> AppManagerLaunchesResults;
			for (mint iAppManager = 0; iAppManager < _nAppManagers; ++iAppManager)
			{
				auto BlockingActorCheckout = fg_BlockingActor();
				auto BlockingActor = BlockingActorCheckout.f_Actor();

				g_Dispatch(BlockingActor) / [=, RootDirectory = State.m_RootDirectory, ProgramDirectory = State.m_ProgramDirectory, BlockingActorCheckout = fg_Move(BlockingActorCheckout)]
					{
						CStr AppManagerDirectory = RootDirectory / ("AppManager{sf0,sl2}"_f << iAppManager);
						CFile::fs_CreateDirectory(AppManagerDirectory);
						CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory / "TestApps/AppManager", AppManagerDirectory, nullptr);
					}
					> AppManagerLaunchesResults
				;
			}
			DMibTestMark;
			co_await fg_AllDone(AppManagerLaunchesResults);
		}
		{
			// Launch AppManagers
			TCFutureMap<mint, CDistributedApp_LaunchInfo> AppManagerLaunchesResults;

			for (mint iAppManager = 0; iAppManager < _nAppManagers; ++iAppManager)
			{
				CStr AppManagerName = "AppManager{sf0,sl2}"_f << iAppManager;
				CStr AppManagerDirectory = State.m_RootDirectory + "/" + AppManagerName;

				TCVector<CStr> ExtraParams;
				if (State.m_Options & EOption_EnableOtherOutput)
					ExtraParams.f_Insert("--log-launches-to-stderr");

				ExtraParams.f_Insert("--auto-update-delay=0.001"); // Make auto update faster

				if (State.m_Options & EOption_DisableDiskMonitoring)
					ExtraParams.f_Insert("--host-monitor-interval=0.0");

				if (State.m_Options & EOption_DisablePatchMonitoring)
					ExtraParams.f_Insert("--host-monitor-patch-interval=0.0");

				if (State.m_Options & EOption_DisableApplicationStatusSensors)
					ExtraParams.f_Insert("--application-status-sensors=false");

				if (State.m_Options & EOption_DisableEncryptionStatusSensors)
					ExtraParams.f_Insert("--encryption-status-sensors=false");

				State.m_LaunchHelper
					(
						&CDistributedApp_LaunchHelper::f_LaunchInProcess
						, AppManagerName
						, AppManagerDirectory
						, &fg_ConstructApp_AppManager
						, fg_Move(ExtraParams)
					)
					> AppManagerLaunchesResults[iAppManager]
				;
			}
			DMibTestMark;
			auto Results = co_await fg_AllDoneWrapped(AppManagerLaunchesResults).f_Timeout(State.m_Timeout, "Timed out waiting for app manager launches");
			for (auto &LaunchResult : Results)
			{
				mint iAppManager = Results.fs_GetKey(LaunchResult);

				auto &AppManager = State.m_AppManagerInfos[LaunchResult->m_HostID];
				State.m_AppManagerHosts[LaunchResult->m_HostID];
				AppManager.m_Launch = fg_Move(*LaunchResult);
				AppManager.m_pTrustInterface = AppManager.m_Launch->m_pTrustInterface;
				AppManager.m_Name = "AppManager{sf0,sl2}"_f << iAppManager;
				AppManager.m_RootDirectory = State.m_RootDirectory + "/" + AppManager.m_Name;
				AppManager.m_Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << AppManager.m_RootDirectory);
			}
		}
		if (State.m_Options & EOption_EnableVersionManager)
		{
			State.m_VersionManagerHostID = State.m_VersionManagerLaunch->m_HostID;

			// Add listen socket that app managers can connect to
			State.m_VersionManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/versionmanager.sock"_f << State.m_VersionManagerDirectory));
			DMibTestMark;
			co_await State.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(State.m_VersionManagerServerAddress)
				.f_Timeout(State.m_Timeout, "Timed out waiting for version manager add listen")
			;
		}
		{
			// Setup CloudMangaer
			State.m_CloudManagerHostID = State.m_CloudManagerLaunch->m_HostID;
			// Add listen socket that app managers can connect to
			State.m_CloudManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/cloudmanager.sock"_f << State.m_CloudManagerDirectory));
			DMibTestMark;
			co_await State.m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(State.m_CloudManagerServerAddress)
				.f_Timeout(State.m_Timeout, "Timed out waiting for cloud manager add listen")
			;
		}
		{
			// Add trust to cloud client
			DMibTestMark;
			State.m_CloudClientHostID = (co_await f_LaunchTool(State.m_CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id"))).f_Trim();
			if (State.m_Options & EOption_EnableVersionManager)
			{
				auto Ticket = co_await State.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{State.m_VersionManagerServerAddress}
					)
					.f_Timeout(State.m_Timeout, "Timed out waiting for version manager ticket generation (cloud client)")
				;
				DMibTestMark;
				co_await f_LaunchTool
					(
						State.m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSONSorted{CVersionManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				co_await State.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(State.m_CloudClientHostID, State.m_VersionManagerPermissionsForTest)
					)
					.f_Timeout(State.m_Timeout, "Timed out waiting for version manager add permission (cloud clien)")
				;
			}
			{
				auto Ticket = co_await State.m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{State.m_CloudManagerServerAddress}
					)
					.f_Timeout(State.m_Timeout, "Timed out waiting for cloud manager generate ticket (cloud client)")
				;
				DMibTestMark;
				co_await f_LaunchTool
					(
						State.m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSONSorted{CCloudManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				co_await State.m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(State.m_CloudClientHostID, State.m_CloudManagerPermissionsForTest)
					)
					.f_Timeout(State.m_Timeout, "Timed out waiting for cloud manager add permission (cloud client)")
				;
			}
		}
		if (State.m_Options & EOption_EnableVersionManager)
		{
			// Setup trust between VersionManager and Test
			DMibTestMark;
			co_await State.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(State.m_TestHostID, State.m_VersionManagerPermissionsForTest)
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for version manager add permission (version manager)")
			;
			DMibTestMark;
			co_await State.m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CVersionManager::mc_pDefaultNamespace
					, TCSet<CStr>{State.m_VersionManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for test allow hosts for namespace (version manager)")
			;
		}
		{
			// Setup trust between CloudManager and Test
			DMibTestMark;
			co_await State.m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(State.m_TestHostID, State.m_CloudManagerPermissionsForTest)
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for cloud manager add permission (test)")
			;
			DMibTestMark;
			co_await State.m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CCloudManager::mc_pDefaultNamespace
					, TCSet<CStr>{State.m_CloudManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for test allow hosts for namespace (cloud manager)")
			;
		}

		if (State.m_Options & EOption_EnableVersionManager)
			State.m_VersionManager = co_await State.m_Subscriptions->f_SubscribeAsync<CVersionManager>();
		State.m_CloudManager = co_await State.m_Subscriptions->f_SubscribeAsync<CCloudManager>();

		{
			State.m_TestAppArchive = State.m_RootDirectory / "TestApp.tar.gz";

			DMibTestMark;

			State.m_PackageInfo = co_await
				(
					g_Dispatch / [VersionManagerHelper = State.m_VersionManagerHelper, Directory = State.m_ProgramDirectory + "/TestApps/TestApp", TestAppArchive = State.m_TestAppArchive]
					{
						return VersionManagerHelper.f_CreatePackage(Directory, TestAppArchive, 1);
					}
				)
				.f_Timeout(State.m_Timeout, "Timed out waiting for create test app package")
			;
			State.m_PackageInfo.m_VersionInfo.m_Tags["TestTag"];

			DMibTestMark;
			if (State.m_Options & EOption_EnableVersionManager)
			{
				co_await
					(
						g_Dispatch
						/ 
						[
							VersionManagerHelper = State.m_VersionManagerHelper
							, VersionManager = State.m_VersionManager
							, PackageInfo = State.m_PackageInfo
							, TestAppArchive = State.m_TestAppArchive
						]()
						-> TCFuture<void>
						{
							co_await VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);
							co_return {};
						}
					)
					.f_Timeout(State.m_Timeout, "Timed out waiting for version manager upload of test package")
				;
			}
		}

		// Setup trust for AppManagers
		{
			TCFutureVector<void> ListenResults;
			for (auto &AppManager : State.m_AppManagerInfos)
				AppManager.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(AppManager.m_Address) > ListenResults;

			DMibTestMark;
			co_await fg_AllDone(ListenResults).f_Timeout(State.m_Timeout, "Timed out waiting for app managers add listen");
		}

		DMibTestMark;
		co_await f_SetupTrust().f_Timeout(State.m_Timeout, "Timed out waiting for setup trust");
		DMibTestMark;
		for (auto &AppManager : co_await State.m_Subscriptions->f_SubscribeMultipleAsync<CAppManagerInterface>(_nAppManagers))
		{
			auto HostID = AppManager->f_GetHostInfo().m_RealHostID;
			State.m_AppManagerInfos[HostID].m_Interface = fg_Move(AppManager);
		}

		DMibTestMark;
		co_await f_InstallTestApp().f_Timeout(State.m_Timeout, "Timed out waiting for install test app");
		DMibTestMark;
		co_await f_CheckCloudManager(0).f_Timeout(State.m_Timeout, "Timed out waiting for check cloud manager");
		DMibTestMark;

		co_return {};
	}
}
