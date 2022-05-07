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

namespace NMib::NCloud
{
	CStr const &CAppManagerTestHelper::CAppManagerInfo::f_GetHostID() const
	{
		return TCMap<CStr, CAppManagerInfo>::fs_GetKey(*this);
	}

	CAppManagerTestHelper::CAppManagerTestHelper(CStr const &_RootDirectory, EOption _Options, fp64 _Timeout)
		: m_RootDirectory(m_ProgramDirectory / _RootDirectory)
		, m_Options(_Options)
		, m_Timeout(_Timeout)
	{
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

	CAppManagerTestHelper::~CAppManagerTestHelper()
	{
		if (m_LaunchHelper)
			m_LaunchHelper->f_BlockDestroy();

		if (m_TrustManager)
			m_TrustManager->f_BlockDestroy();
	}

	TCFuture<NStr::CStr> CAppManagerTestHelper::f_LaunchTool
		(
			NStr::CStr _Executable
			, NContainer::TCVector<NStr::CStr> _Params
			, NStr::CStr _WorkingDir
		)
	{
		co_return
			(
				co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(_Executable, _Params, _WorkingDir))
				.f_Timeout(m_Timeout, "Timed out waiting for tool launch: {} {vs}"_f << _Executable << _Params)
			)
			.f_GetStdOut()
		;
	}

	TCFuture<void> CAppManagerTestHelper::f_SetupTrust()
	{
		TCActorResultVector<void> SetupTrustResults;

		// Trust between app managers
		for (auto &AppManager : m_AppManagerInfos)
		{
			auto pAppManagerTrust = AppManager.m_pTrustInterface;
			auto &AppManagerTrust = *pAppManagerTrust;
			CStr AppManagerHostID = AppManager.f_GetHostID();
			auto TrustAppManagers = m_AppManagerHosts;
			TrustAppManagers.f_Remove(AppManagerHostID);
			if (!TrustAppManagers.f_IsEmpty())
			{
				AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						fs_NamespaceHosts("com.malterlib/Cloud/AppManagerCoordination", TrustAppManagers)
					)
					> SetupTrustResults.f_AddResult()
				;
			}

			m_TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
				(
					CAppManagerInterface::mc_pDefaultNamespace
					, TCSet<CStr>{AppManagerHostID}
					, mc_WaitForSubscriptions
				)
				> SetupTrustResults.f_AddResult()
			;

			for (auto &AppManagerInner : m_AppManagerInfos)
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
					> Promise / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
					{
						auto &AppManagerTrustInner = *pAppManagerTrustInner;
						AppManagerTrustInner.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, m_Timeout, -1) > Promise.f_ReceiveAny();
					}
				;
				Promise.f_MoveFuture() > SetupTrustResults.f_AddResult();
			}
		}

		for (auto &AppManager : m_AppManagerInfos)
		{
			auto pAppManagerTrust = AppManager.m_pTrustInterface;
			auto &AppManagerTrust = *pAppManagerTrust;
			auto pCloudManagerTrust = m_CloudManagerLaunch->m_pTrustInterface;
			auto &CloudManagerTrust = *pCloudManagerTrust;
			CStr AppManagerHostID = AppManager.f_GetHostID();

			if (m_Options & EOption_EnableVersionManager)
			{
				auto pVersionManagerTrust = m_VersionManagerLaunch->m_pTrustInterface;
				auto &VersionManagerTrust = *pVersionManagerTrust;
				TCPromise<void> Promise;

				VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_VersionManagerServerAddress}
					)
					> Promise / [=, VersionManagerHostID = m_VersionManagerHostID]
					(
						CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_VersionManagerTicket
					)
					{
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &VersionManagerTrust = *pVersionManagerTrust;
						AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_VersionManagerTicket.m_Ticket, m_Timeout, -1)
							+ VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fs_Permissions(AppManagerHostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll"}})
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
				Promise.f_MoveFuture() > SetupTrustResults.f_AddResult();
			}

			{
				TCPromise<void> Promise;

				CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_CloudManagerServerAddress}
					)
					+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_TestHostID, TCMap<CStr, CPermissionRequirements>{{"AppManager/VersionAppAll"}, {"AppManager/CommandAll"}, {"AppManager/AppAll"}})
					)
					+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_CloudManagerHostID, TCMap<CStr, CPermissionRequirements>{{"AppManager/Command/ApplicationSubscribeChanges"}, {"AppManager/AppAll"}})
					)
					> Promise / [=, CloudManagerHostID = m_CloudManagerHostID]
					(
						CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_CloudManagerTicket
						, CVoidTag
						, CVoidTag
					)
					{
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &CloudManagerTrust = *pCloudManagerTrust;
						AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_CloudManagerTicket.m_Ticket, m_Timeout, -1)
							+ CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fs_Permissions
								(
									AppManagerHostID
									, TCMap<CStr, CPermissionRequirements>
									{
										{"CloudManager/RegisterAppManager"}
										, {"CloudManager/ReportSensorReadings"}
										, {"CloudManager/ReportSensorReadingsOnBehalfOf/All"}
										, {"CloudManager/ReportLogEntries"}
										, {"CloudManager/ReportLogEntriesOnBehalfOf/All"}
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
				Promise.f_MoveFuture() > SetupTrustResults.f_AddResult();
			}
		}
		DMibTestMark;
		co_await SetupTrustResults.f_GetResults().f_Timeout(m_Timeout, "Timed out waiting for trust setup") | g_Unwrap;

		DMibTestMark;
		for (NTime::CClock Timer(true); true; co_await fg_Timeout(1.0))
		{
			bool bAllFinished = true;
			CStr VersionManagers;
			CStr CloudManagers;
			for (auto &AppManager : m_AppManagerInfos)
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

			if (Timer.f_GetTime() > m_Timeout / 2)
				DMibError("Timed out waiting for version manager and cloud manager subscriptions:\nVersionManagers: {vs}\nCloudManagers: {vs}"_f << VersionManagers << CloudManagers);
		}
		DMibTestMark;

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_InstallTestApp(CStr _Name, CStr _Tag, CStr _Group, CStr _VersionManagerApplication)
	{
		TCActorResultVector<void> AddAppResults;
		for (auto &AppManager : m_AppManagerInfos)
		{
			if (m_Options & EOption_LaunchTestAppInApp)
			{
				if (_VersionManagerApplication == "TestApp")
				{
					CStr ExecutableName = AppManager.m_RootDirectory / "App" / _Name / "TestApp";
#ifdef DPlatformFamily_Windows
					ExecutableName += ".exe";
#endif
					m_InProcessLaunchScopes.f_Insert(fg_AppManager_RegisterInProcessFactory(ExecutableName, &fg_ConstructApp_TestApp));
				}
			}

			if (m_Options & EOption_EnableVersionManager)
			{
				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings Settings;
				Settings.m_VersionManagerApplication = _VersionManagerApplication;
				Settings.m_bAutoUpdate = true;
				Settings.m_UpdateTags = TCSet<CStr>{_Tag};
				Settings.m_UpdateGroup = _Group;
				Settings.m_bLaunchInProcess = (m_Options & EOption_LaunchTestAppInApp) != EOption_None;
				Add.m_Version = m_PackageInfo.m_VersionID;

				AppManager.m_Interface.f_CallActor(&CAppManagerInterface::f_Add)(_Name, Add, Settings) > AddAppResults.f_AddResult();
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
						, m_TestAppArchive
						, "--name"
						, _Name
						, "--update-tags"
						, _Tag
						, "--update-group"
						, _Group
					}
				;

				if (m_Options & EOption_LaunchTestAppInApp)
					Params.f_Insert("--launch-in-process");

				TCPromise<void> LaunchPromise;
				f_LaunchTool(AppManager.m_RootDirectory / "AppManager", Params, AppManager.m_RootDirectory) > LaunchPromise.f_ReceiveAny();
				LaunchPromise.f_Future() > AddAppResults.f_AddResult();
			}
		}
		DMibTestMark;
		co_await AddAppResults.f_GetResults().f_Timeout(m_Timeout, "Timed out waiting for installing test apps") | g_Unwrap;

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_CheckCloudManager(mint _Sequence)
	{
		DMibTestPath("CloudManager{}"_f << _Sequence);

		auto AppManagers = co_await m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_Timeout(m_Timeout, "Timed out waiting for app manager enumeration 1");
		NTime::CClock Clock{true};
		while (AppManagers.f_GetLen() < m_nAppManagers && Clock.f_GetTime() < m_Timeout)
			AppManagers = co_await m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_Timeout(m_Timeout, "Timed out waiting for app manager enumeration 2");
		DMibExpect(AppManagers.f_GetLen(), ==, m_nAppManagers);

		NStr::CStr HostName = NProcess::NPlatform::fg_Process_GetFullyQualiedHostName();
		TCSet<CStr> ExpectedAppManagers;
		for (auto &Info : m_AppManagerInfos)
			ExpectedAppManagers[("{}/{}:{}"_f << Info.f_GetHostID() << HostName << (Info.m_RootDirectory)).f_GetStr()];

		TCSet<CStr> ActualAppManagers;
		for (auto &AppManager : AppManagers)
			ActualAppManagers[("{}/{}:{}"_f << AppManagers.fs_GetKey(AppManager) << AppManager.m_HostName << AppManager.m_ProgramDirectory).f_GetStr()];

		DMibExpect(ActualAppManagers, ==, ExpectedAppManagers);

		auto Applications = co_await m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_Timeout(m_Timeout, "Timed out waiting for app manager enumeration 3");
		while (Applications.f_GetLen() < m_nAppManagers && Clock.f_GetTime() < m_Timeout)
			Applications = co_await m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_Timeout(m_Timeout, "Timed out waiting for app manager enumeration 4");
		DMibExpect(Applications.f_GetLen(), ==, m_nAppManagers);

		for (auto &Application : Applications)
		{
			auto &ApplicationKey = Applications.fs_GetKey(Application);
			DMibExpect(ApplicationKey.m_Name, ==, "TestApp")(ETestFlag_Aggregated);
			DMibExpect(Application.m_ApplicationInfo.m_Status, ==, "Launched")(ETestFlag_Aggregated);
		}

		co_return {};
	}

	TCFuture<void> CAppManagerTestHelper::f_Setup(mint _nAppManagers)
	{
		m_nAppManagers = _nAppManagers;
		CProcessLaunch::fs_KillProcessesInDirectory("*", {}, m_RootDirectory, m_Timeout);

		for (mint i = 0; i < 5; ++i)
		{
			try
			{
				if (CFile::fs_FileExists(m_RootDirectory))
					CFile::fs_DeleteDirectoryRecursive(m_RootDirectory);
				break;
			}
			catch (NFile::CExceptionFile const &)
			{
			}
		}

		CFile::fs_CreateDirectory(m_RootDirectory);

		m_TrustManager = m_TrustManagerState.f_TrustManager("TestHelper");
		m_TestHostID = co_await m_TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_Timeout(m_Timeout, "Timed out waiting for host id of trust manager");
		m_Subscriptions = fg_Construct(m_TrustManager);

		m_ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << m_RootDirectory);
		co_await m_TrustManager(&CDistributedActorTrustManager::f_AddListen, m_ServerAddress).f_Timeout(m_Timeout, "Timed out waiting for listen addition");

		{
			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = m_ServerAddress.m_URL;
			Dependencies.m_TrustManager = m_TrustManager;
			Dependencies.m_DistributionManager = co_await m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager)
					.f_Timeout(m_Timeout, "Timed out waiting for distribution manager")
			;

			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CCloudManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CDistributedAppSensorReporter::mc_pDefaultNamespace);

			co_await Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_Timeout(m_Timeout, "Timed out waiting for set security");

			m_LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, (m_Options & EOption_EnableOtherOutput) != EOption_None);
		}
		if (m_Options & EOption_EnableVersionManager)
		{
			// Launch VersionManager
			m_VersionManagerDirectory = m_RootDirectory + "/VersionManager";
			CFile::fs_CreateDirectory(m_VersionManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(m_ProgramDirectory + "/TestApps/VersionManager", m_VersionManagerDirectory, nullptr);

			m_VersionManagerLaunch = co_await m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager"
					, m_VersionManagerDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(m_Timeout, "Timed out waiting for version manager launch")
			;

			DMibExpect(m_VersionManagerLaunch->m_HostID, !=, "");

		}
		{
			// Launch CloudManager
			m_CloudManagerDirectory = m_RootDirectory + "/CloudManager";
			CFile::fs_CreateDirectory(m_CloudManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(m_ProgramDirectory + "/TestApps/CloudManager", m_CloudManagerDirectory, nullptr);

			NCloudManager::g_MaxDatabaseSize = constant_uint64(1) * 1024 * 1024 * 1024; // Limit due to asan for example taking too much memory

			m_CloudManagerLaunch = co_await m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "CloudManager"
					, m_CloudManagerDirectory
					, &fg_ConstructApp_CloudManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(m_Timeout, "Timed out waiting for cloud manager launch")
			;

			DMibExpect(m_CloudManagerLaunch->m_HostID, !=, "");
		}
		{
			// Copy Cloud Client for debugging
			m_CloudClientDirectory = m_RootDirectory / "MalterlibCloud";
			CFile::fs_CreateDirectory(m_CloudClientDirectory);
			CFile::fs_DiffCopyFileOrDirectory(m_ProgramDirectory / "TestApps/MalterlibCloud", m_CloudClientDirectory, nullptr);
		}
		// Copy AppManagers to their directories
		{
			TCActorResultVector<void> AppManagerLaunchesResults;
			TCVector<TCActor<CSeparateThreadActor>> FileActors;
			for (mint iAppManager = 0; iAppManager < _nAppManagers; ++iAppManager)
			{
				auto &FileActor = FileActors.f_Insert() = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
				g_Dispatch(FileActor) / [=, RootDirectory = m_RootDirectory, ProgramDirectory = m_ProgramDirectory]
					{
						CStr AppManagerDirectory = RootDirectory / ("AppManager{sf0,sl2}"_f << iAppManager);
						CFile::fs_CreateDirectory(AppManagerDirectory);
						CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory / "TestApps/AppManager", AppManagerDirectory, nullptr);
					}
					> AppManagerLaunchesResults.f_AddResult()
				;
			}
			DMibTestMark;
			co_await AppManagerLaunchesResults.f_GetResults() | g_Unwrap;
		}
		{
			// Launch AppManagers
			TCActorResultMap<mint, CDistributedApp_LaunchInfo> AppManagerLaunchesResults;

			for (mint iAppManager = 0; iAppManager < _nAppManagers; ++iAppManager)
			{
				CStr AppManagerName = "AppManager{sf0,sl2}"_f << iAppManager;
				CStr AppManagerDirectory = m_RootDirectory + "/" + AppManagerName;

				TCVector<CStr> ExtraParams;
				if (m_Options & EOption_EnableOtherOutput)
					ExtraParams.f_Insert("--log-launches-to-stderr");

				ExtraParams.f_Insert("--auto-update-delay=1.0"); // Make auto update faster
				ExtraParams.f_Insert("--host-monitor-interval=0.0"); // Disable automatic disk space monitoring update

				m_LaunchHelper
					(
						&CDistributedApp_LaunchHelper::f_LaunchInProcess
						, AppManagerName
						, AppManagerDirectory
						, &fg_ConstructApp_AppManager
						, fg_Move(ExtraParams)
					)
					> AppManagerLaunchesResults.f_AddResult(iAppManager)
				;
			}
			DMibTestMark;
			auto Results = co_await AppManagerLaunchesResults.f_GetResults().f_Timeout(m_Timeout, "Timed out waiting for app manager launches");
			for (auto &LaunchResult : Results)
			{
				mint iAppManager = Results.fs_GetKey(LaunchResult);

				auto &AppManager = m_AppManagerInfos[LaunchResult->m_HostID];
				m_AppManagerHosts[LaunchResult->m_HostID];
				AppManager.m_Launch = fg_Move(*LaunchResult);
				AppManager.m_pTrustInterface = AppManager.m_Launch->m_pTrustInterface;
				AppManager.m_Name = "AppManager{sf0,sl2}"_f << iAppManager;
				AppManager.m_RootDirectory = m_RootDirectory + "/" + AppManager.m_Name;
				AppManager.m_Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << AppManager.m_RootDirectory);
			}
		}
		if (m_Options & EOption_EnableVersionManager)
		{
			m_VersionManagerHostID = m_VersionManagerLaunch->m_HostID;

			// Add listen socket that app managers can connect to
			m_VersionManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/versionmanager.sock"_f << m_VersionManagerDirectory));
			DMibTestMark;
			co_await m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(m_VersionManagerServerAddress)
				.f_Timeout(m_Timeout, "Timed out waiting for version manager add listen")
			;
		}
		{
			// Setup CloudMangaer
			m_CloudManagerHostID = m_CloudManagerLaunch->m_HostID;
			// Add listen socket that app managers can connect to
			m_CloudManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/cloudmanager.sock"_f << m_CloudManagerDirectory));
			DMibTestMark;
			co_await m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(m_CloudManagerServerAddress)
				.f_Timeout(m_Timeout, "Timed out waiting for cloud manager add listen")
			;
		}
		{
			// Add trust to cloud client
			DMibTestMark;
			m_CloudClientHostID = (co_await f_LaunchTool(m_CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id"))).f_Trim();
			if (m_Options & EOption_EnableVersionManager)
			{
				auto Ticket = co_await m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_VersionManagerServerAddress}
					)
					.f_Timeout(m_Timeout, "Timed out waiting for version manager ticket generation (cloud client)")
				;
				DMibTestMark;
				co_await f_LaunchTool
					(
						m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSON{CVersionManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				co_await m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_CloudClientHostID, m_VersionManagerPermissionsForTest)
					)
					.f_Timeout(m_Timeout, "Timed out waiting for version manager add permission (cloud clien)")
				;
			}
			{
				auto Ticket = co_await m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_CloudManagerServerAddress}
					)
					.f_Timeout(m_Timeout, "Timed out waiting for cloud manager generate ticket (cloud client)")
				;
				DMibTestMark;
				co_await f_LaunchTool
					(
						m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSON{CCloudManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				co_await m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_CloudClientHostID, m_CloudManagerPermissionsForTest)
					)
					.f_Timeout(m_Timeout, "Timed out waiting for cloud manager add permission (cloud client)")
				;
			}
		}
		if (m_Options & EOption_EnableVersionManager)
		{
			// Setup trust between VersionManager and Test
			DMibTestMark;
			co_await m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(m_TestHostID, m_VersionManagerPermissionsForTest)
				)
				.f_Timeout(m_Timeout, "Timed out waiting for version manager add permission (version manager)")
			;
			DMibTestMark;
			co_await m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CVersionManager::mc_pDefaultNamespace
					, TCSet<CStr>{m_VersionManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_Timeout(m_Timeout, "Timed out waiting for test allow hosts for namespace (version manager)")
			;
		}
		{
			// Setup trust between CloudManager and Test
			DMibTestMark;
			co_await m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(m_TestHostID, m_CloudManagerPermissionsForTest)
				)
				.f_Timeout(m_Timeout, "Timed out waiting for cloud manager add permission (test)")
			;
			DMibTestMark;
			co_await m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CCloudManager::mc_pDefaultNamespace
					, TCSet<CStr>{m_CloudManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_Timeout(m_Timeout, "Timed out waiting for test allow hosts for namespace (cloud manager)")
			;
		}

		if (m_Options & EOption_EnableVersionManager)
			m_VersionManager = co_await m_Subscriptions->f_SubscribeAsync<CVersionManager>();
		m_CloudManager = co_await m_Subscriptions->f_SubscribeAsync<CCloudManager>();

		{
			m_TestAppArchive = m_RootDirectory / "TestApp.tar.gz";

			DMibTestMark;

			m_PackageInfo = co_await
				(
					g_Dispatch / [VersionManagerHelper = m_VersionManagerHelper, Directory = m_ProgramDirectory + "/TestApps/TestApp", TestAppArchive = m_TestAppArchive]
					{
						return VersionManagerHelper.f_CreatePackage(Directory, TestAppArchive, 1);
					}
				)
				.f_Timeout(m_Timeout, "Timed out waiting for create test app package")
			;
			m_PackageInfo.m_VersionInfo.m_Tags["TestTag"];

			DMibTestMark;
			if (m_Options & EOption_EnableVersionManager)
			{
				co_await
					(
						g_Dispatch
						/ [VersionManagerHelper = m_VersionManagerHelper,VersionManager = m_VersionManager, PackageInfo = m_PackageInfo, TestAppArchive = m_TestAppArchive]()
						-> TCFuture<void>
						{
							co_await VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);
							co_return {};
						}
					)
					.f_Timeout(m_Timeout, "Timed out waiting for version manager upload of test package")
				;
			}
		}

		// Setup trust for AppManagers
		{
			TCActorResultVector<void> ListenResults;
			for (auto &AppManager : m_AppManagerInfos)
				AppManager.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(AppManager.m_Address) > ListenResults.f_AddResult();

			DMibTestMark;
			co_await ListenResults.f_GetResults().f_Timeout(m_Timeout, "Timed out waiting for app managers add listen") | g_Unwrap;
		}

		DMibTestMark;
		co_await f_SetupTrust().f_Timeout(m_Timeout, "Timed out waiting for setup trust");
		DMibTestMark;
		for (auto &AppManager : co_await m_Subscriptions->f_SubscribeMultipleAsync<CAppManagerInterface>(_nAppManagers))
		{
			auto HostID = AppManager->f_GetHostInfo().m_RealHostID;
			m_AppManagerInfos[HostID].m_Interface = fg_Move(AppManager);
		}

		DMibTestMark;
		co_await f_InstallTestApp().f_Timeout(m_Timeout, "Timed out waiting for install test app");
		DMibTestMark;
		co_await f_CheckCloudManager(0).f_Timeout(m_Timeout, "Timed out waiting for check cloud manager");
		DMibTestMark;

		co_return {};
	}
}
