// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"
#include <Mib/Cloud/App/TestApp>

#ifdef DPlatformFamily_Windows
#include "Windows.h"
#endif

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
		if (m_Options & EOption_EnableLogging)
			fg_GetSys()->f_AddStdErrLogger();
	}

	CAppManagerTestHelper::~CAppManagerTestHelper()
	{
		if (m_HelperActor)
			m_HelperActor->f_BlockDestroy();

		if (m_LaunchHelper)
			m_LaunchHelper->f_BlockDestroy();

		if (m_TrustManager)
			m_TrustManager->f_BlockDestroy();
	}

	void CAppManagerTestHelper::f_SetupTrust()
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
		fg_CombineResults(SetupTrustResults.f_GetResults().f_CallSync(m_Timeout));
	}

	void CAppManagerTestHelper::f_InstallTestApp(CStr const &_Name, CStr const &_Tag, CStr const &_Group, CStr const &_VersionManagerApplication)
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

				CProcessLaunch::fs_LaunchTool(AppManager.m_RootDirectory / "AppManager", Params, AppManager.m_RootDirectory);
			}
		}
		DMibTestMark;
		fg_CombineResults(AddAppResults.f_GetResults().f_CallSync(m_Timeout));
	}

	void CAppManagerTestHelper::f_CheckCloudManager(mint _Sequence)
	{
		DMibTestPath("CloudManager{}"_f << _Sequence);

		auto AppManagers = m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_CallSync(m_Timeout);
		NTime::CClock Clock{true};
		while (AppManagers.f_GetLen() < m_nAppManagers && Clock.f_GetTime() < m_Timeout)
			AppManagers = m_CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_CallSync(m_Timeout);
		DMibExpect(AppManagers.f_GetLen(), ==, m_nAppManagers);

		NStr::CStr HostName = NProcess::NPlatform::fg_Process_GetFullyQualiedHostName();
		TCSet<CStr> ExpectedAppManagers;
		for (auto &Info : m_AppManagerInfos)
			ExpectedAppManagers[("{}/{}:{}"_f << Info.f_GetHostID() << HostName << (Info.m_RootDirectory)).f_GetStr()];

		TCSet<CStr> ActualAppManagers;
		for (auto &AppManager : AppManagers)
			ActualAppManagers[("{}/{}:{}"_f << AppManagers.fs_GetKey(AppManager) << AppManager.m_HostName << AppManager.m_ProgramDirectory).f_GetStr()];

		DMibExpect(ActualAppManagers, ==, ExpectedAppManagers);

		auto Applications = m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_CallSync(m_Timeout);
		while (Applications.f_GetLen() < m_nAppManagers && Clock.f_GetTime() < m_Timeout)
			Applications = m_CloudManager.f_CallActor(&CCloudManager::f_EnumApplications)().f_CallSync(m_Timeout);
		DMibExpect(Applications.f_GetLen(), ==, m_nAppManagers);

		for (auto &Application : Applications)
		{
			auto &ApplicationKey = Applications.fs_GetKey(Application);
			DMibExpect(ApplicationKey.m_Name, ==, "TestApp")(ETestFlag_Aggregated);
			DMibExpect(Application.m_ApplicationInfo.m_Status, ==, "Launched")(ETestFlag_Aggregated);
		}
	}

	void CAppManagerTestHelper::f_Setup(mint _nAppManagers)
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
		m_TestHostID = m_TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(m_Timeout);
		m_Subscriptions = fg_Construct(m_TrustManager);

		m_ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << m_RootDirectory);
		m_TrustManager(&CDistributedActorTrustManager::f_AddListen, m_ServerAddress).f_CallSync(m_Timeout);

		{
			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = m_ServerAddress.m_URL;
			Dependencies.m_TrustManager = m_TrustManager;
			Dependencies.m_DistributionManager = m_TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(m_Timeout);

			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CCloudManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CDistributedAppSensorReporter::mc_pDefaultNamespace);

			Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(m_Timeout);

			m_LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, (m_Options & EOption_EnableLogging | EOption_EnableOtherOutput) != EOption_None);
		}
		if (m_Options & EOption_EnableVersionManager)
		{
			// Launch VersionManager
			m_VersionManagerDirectory = m_RootDirectory + "/VersionManager";
			CFile::fs_CreateDirectory(m_VersionManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(m_ProgramDirectory + "/TestApps/VersionManager", m_VersionManagerDirectory, nullptr);

			m_VersionManagerLaunch = m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager"
					, m_VersionManagerDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(m_Timeout)
			;

			DMibExpect(m_VersionManagerLaunch->m_HostID, !=, "");

		}
		{
			// Launch CloudManager
			m_CloudManagerDirectory = m_RootDirectory + "/CloudManager";
			CFile::fs_CreateDirectory(m_CloudManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(m_ProgramDirectory + "/TestApps/CloudManager", m_CloudManagerDirectory, nullptr);

			m_CloudManagerLaunch = m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "CloudManager"
					, m_CloudManagerDirectory
					, &fg_ConstructApp_CloudManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(m_Timeout)
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
			fg_CombineResults(AppManagerLaunchesResults.f_GetResults().f_CallSync());
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
			auto Results = AppManagerLaunchesResults.f_GetResults().f_CallSync(m_Timeout);
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
			m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(m_VersionManagerServerAddress).f_CallSync(m_Timeout);
		}
		{
			// Setup CloudMangaer
			m_CloudManagerHostID = m_CloudManagerLaunch->m_HostID;
			// Add listen socket that app managers can connect to
			m_CloudManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/cloudmanager.sock"_f << m_CloudManagerDirectory));
			DMibTestMark;
			m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(m_CloudManagerServerAddress).f_CallSync(m_Timeout);
		}
		{
			// Add trust to cloud client
			DMibTestMark;
			m_CloudClientHostID = CProcessLaunch::fs_LaunchTool(m_CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
			if (m_Options & EOption_EnableVersionManager)
			{
				auto Ticket = m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_VersionManagerServerAddress}
					)
					.f_CallSync(m_Timeout)
				;
				CProcessLaunch::fs_LaunchTool
					(
						m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSON{CVersionManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_CloudClientHostID, m_VersionManagerPermissionsForTest)
					)
					.f_CallSync(m_Timeout)
				;
			}
			{
				auto Ticket = m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_CloudManagerServerAddress}
					)
					.f_CallSync(m_Timeout)
				;
				CProcessLaunch::fs_LaunchTool
					(
						m_CloudClientDirectory + "/MalterlibCloud"
						, {"--trust-connection-add", "--trusted-namespaces", CJSON{CCloudManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
					)
				;
				DMibTestMark;
				m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fs_Permissions(m_CloudClientHostID, m_CloudManagerPermissionsForTest)
					)
					.f_CallSync(m_Timeout)
				;
			}
		}
		if (m_Options & EOption_EnableVersionManager)
		{
			// Setup trust between VersionManager and Test
			DMibTestMark;
			m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(m_TestHostID, m_VersionManagerPermissionsForTest)
				)
				.f_CallSync(m_Timeout)
			;
			DMibTestMark;
			m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CVersionManager::mc_pDefaultNamespace
					, TCSet<CStr>{m_VersionManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_CallSync(m_Timeout)
			;
		}
		{
			// Setup trust between CloudManager and Test
			DMibTestMark;
			m_CloudManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fs_Permissions(m_TestHostID, m_CloudManagerPermissionsForTest)
				)
				.f_CallSync(m_Timeout)
			;
			DMibTestMark;
			m_TrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CCloudManager::mc_pDefaultNamespace
					, TCSet<CStr>{m_CloudManagerHostID}
					, mc_WaitForSubscriptions
				)
				.f_CallSync(m_Timeout)
			;
		}

		if (m_Options & EOption_EnableVersionManager)
			m_VersionManager = m_Subscriptions->f_Subscribe<CVersionManager>();
		m_CloudManager = m_Subscriptions->f_Subscribe<CCloudManager>();

		{
			m_TestAppArchive = m_ProgramDirectory / "TestApps/TestApp.tar.gz";

			DMibTestMark;

			m_PackageInfo =
				(
					g_Dispatch(m_HelperActor) /
					[VersionManagerHelper = m_VersionManagerHelper, Directory = m_ProgramDirectory + "/TestApps/TestApp", TestAppArchive = m_TestAppArchive]
					{
						return VersionManagerHelper.f_CreatePackage(Directory, TestAppArchive, 1);
					}
				)
				.f_CallSync(m_Timeout)
			;
			m_PackageInfo.m_VersionInfo.m_Tags["TestTag"];

			DMibTestMark;
			if (m_Options & EOption_EnableVersionManager)
			{
				(
					g_Dispatch(m_HelperActor) /
					[VersionManagerHelper = m_VersionManagerHelper,VersionManager = m_VersionManager, PackageInfo = m_PackageInfo, TestAppArchive = m_TestAppArchive]() -> TCFuture<void>
					{
						co_await VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);
						co_return {};
					}
				)
				.f_CallSync(m_Timeout);
			}
		}

		// Setup trust for AppManagers
		{
			TCActorResultVector<void> ListenResults;
			for (auto &AppManager : m_AppManagerInfos)
				AppManager.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(AppManager.m_Address) > ListenResults.f_AddResult();

			DMibTestMark;
			fg_CombineResults(ListenResults.f_GetResults().f_CallSync(m_Timeout));
		}

		f_SetupTrust();
		for (auto &AppManager : m_Subscriptions->f_SubscribeMultiple<CAppManagerInterface>(_nAppManagers))
		{
			auto HostID = AppManager->f_GetHostInfo().m_RealHostID;
			m_AppManagerInfos[HostID].m_Interface = fg_Move(AppManager);
		}

		f_InstallTestApp();
		f_CheckCloudManager(0);
	}
}
