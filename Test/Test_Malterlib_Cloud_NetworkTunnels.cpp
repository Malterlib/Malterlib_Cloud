// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cloud/NetworkTunnels>
#include <Mib/Cloud/NetworkTunnelsServer>
#include <Mib/Cloud/NetworkTunnelsClient>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/DistributedAppLaunchHelper>
#include <Mib/Encoding/JSONShortcuts>

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NCloud;
using namespace NMib::NStorage;
using namespace NMib::NAtomic;
using namespace NMib::NEncoding;
using namespace NMib::NStorage;
using namespace NMib::NTime;
using namespace NMib::NNetwork;
using namespace NMib::NTest;

static fp64 g_Timeout = NSys::fg_System_BeingDebugged() ? 600.0 : 60.0 * gc_TimeoutMultiplier;

struct CNetworkTunnel_Tests : public NMib::NTest::CTest
{
	struct CTestNetworkTunnelServerApp : public CDistributedAppActor
	{
		CTestNetworkTunnelServerApp(CStr const &_ListenPath)
			: CDistributedAppActor
			(
				CDistributedAppActor_Settings("TestNetworkTunnelServerApp")
				.f_SeparateDistributionManager(true)
				.f_KeySetting(NConcurrency::CDistributedActorTestKeySettings{})
				.f_DefaultCommandLineFunctionalies(EDefaultCommandLineFunctionality_None)
			)
			, m_ListenPath(_ListenPath)
		{
		}

		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override
		{
			m_TunnelServer = fg_Construct
				(
					mp_State.m_DistributionManager
					, mp_State.m_TrustManager
					, mp_State.f_AuditorFactory()
					, "TestNetworkTunnelServerApp"
					, "TunnelServerApp"
				)
			;

			m_PublishedTunnels.f_Insert(co_await m_TunnelServer(&CNetworkTunnelsServer::f_PublishNetworkTunnel, "TestTunnel", m_ListenPath, 0, CEJSONSorted{}));

			co_await m_TunnelServer(&CNetworkTunnelsServer::f_Start);

			co_return {};
		}

		TCFuture<void> fp_StopApp() override
		{
			TCActorResultVector<void> Destroys;
			for (auto &Tunnel : m_PublishedTunnels)
				Tunnel->f_Destroy() > Destroys.f_AddResult();

			co_await Destroys.f_GetResults();

			if (m_TunnelServer)
				co_await m_TunnelServer.f_Destroy();

			co_return {};
		}

		TCActor<CNetworkTunnelsServer> m_TunnelServer;
		TCVector<CActorSubscription> m_PublishedTunnels;
		CStr m_ListenPath;
	};

	struct CTestNetworkTunnelClientApp : public CDistributedAppActor
	{
		CTestNetworkTunnelClientApp()
			: CDistributedAppActor
			(
				CDistributedAppActor_Settings("TestNetworkTunnelClientApp")
				.f_SeparateDistributionManager(true)
				.f_KeySetting(NConcurrency::CDistributedActorTestKeySettings{})
				.f_DefaultCommandLineFunctionalies(EDefaultCommandLineFunctionality_None)
			)
		{
		}

		TCFuture<void> fp_StartApp(NEncoding::CEJSONSorted const &_Params) override
		{
			m_TunnelClient = fg_Construct(mp_State.m_DistributionManager, mp_State.m_TrustManager);

			co_await m_TunnelClient(&CNetworkTunnelsClient::f_Start);

			co_return {};
		}

		TCFuture<void> fp_StopApp() override
		{
			TCActorResultVector<void> Destroys;
			for (auto &Subscription : m_TunnelSubscriptions)
				Subscription->f_Destroy() > Destroys.f_AddResult();

			co_await Destroys.f_GetResults();

			if (m_TunnelClient)
				co_await m_TunnelClient.f_Destroy();

			co_return {};
		}

		TCFuture<CEJSONSorted> fp_Test_Command(NStr::CStr const &_Command, NEncoding::CEJSONSorted const &_Params) override
		{
			if (_Command == "OpenTunnel")
			{
				auto Tunnel = co_await m_TunnelClient
					(
						&CNetworkTunnelsClient::f_OpenTunnel
						, _Params["HostID"].f_String()
						, _Params["TunnelName"].f_String()
						, g_ActorFunctor / [](CNetAddress const &_Address) -> TCFuture<void> // New connection
						{
							if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
								DMibConErrOut2("New connection: {}\n", _Address.f_GetString());
							co_return {};
						}
						, g_ActorFunctor / [](CNetAddress const &_Address, NStr::CStr const &_Message) -> TCFuture<void> // On Close
						{
							if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
								DMibConErrOut2("Connection from '{}' closed: {}\n", _Address.f_GetString(), _Message);
							co_return {};
						}
						, g_ActorFunctor / [](CNetAddress const &_Address, CStr const &_Error) -> TCFuture<void> // On Error
						{
							if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
								DMibConErrOut2("Connection from '{}' error: {}\n", _Address.f_GetString(), _Error);
							co_return {};
						}
						, _Params["ListenHost"].f_String()
					)
				;

				m_TunnelSubscriptions.f_Insert(fg_Move(Tunnel.m_Subscription));

				co_return {};
			}
			else if (_Command == "EnumTunnels")
			{
				auto Tunnels = co_await m_TunnelClient(&CNetworkTunnelsClient::f_EnumTunnels);

				CEJSONSorted Return;

				auto &OutTunnels = Return["Tunnels"] = EJSONType_Object;

				for (auto &TunnelsForHost : Tunnels)
				{
					auto &OutHost = OutTunnels[Tunnels.fs_GetKey(TunnelsForHost)].f_Array();
					for (auto &Tunnel : TunnelsForHost)
					{
						auto &TunnelName = TunnelsForHost.fs_GetKey(Tunnel);
						OutHost.f_Insert(TunnelName);
					}
				}

				co_return fg_Move(Return);
			}
			co_return {};
		}

		TCVector<CActorSubscription> m_TunnelSubscriptions;
		TCActor<CNetworkTunnelsClient> m_TunnelClient;
	};

	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			CActorRunLoopTestHelper RunLoopHelper;
			
			CStr SocketPath = CFile::fs_GetProgramDirectory() / "Sockets/NetworkTunnelGeneral";

			CDistributedActorTestHelperCombined TestServer{SocketPath, RunLoopHelper.m_pRunLoop};
			TestServer.f_InitServer();

			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory + "/NetworkTunnelTests";

			fg_TestAddCleanupPath(RootDirectory);


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

			CTrustManagerTestHelper TrustManagerState;
			TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
			auto CleanupTrustManager = g_OnScopeExit / [&]
				{
					TrustManager->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
				}
			;

			CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};

			CDistributedActorTrustManager_Address ServerAddress;

			ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
			TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = ServerAddress.m_URL;
			Dependencies.m_TrustManager = TrustManager;
			Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

			TCActor<CDistributedApp_LaunchHelper> LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, !!(fg_TestReportFlags() & ETestReportFlag_EnableLogs));
			auto Cleanup = g_OnScopeExit / [&]
				{
					LaunchHelper->f_BlockDestroy(RunLoopHelper.m_pRunLoop->f_ActorDestroyLoop());
				}
			;

			// Launch Tunnel Server
			CStr TunnelServerDirectory = RootDirectory + "/TunnelServer";
			CFile::fs_CreateDirectory(TunnelServerDirectory);

			auto TunnelServerLaunch = LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "TunnelServer"
					, TunnelServerDirectory
					, [Path = TestServer.f_GetListenSocketPath()]() -> TCActor<CDistributedAppActor>
					{
						return fg_Construct<CTestNetworkTunnelServerApp>(Path);
					}
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
			;

			//auto &TunnelServerApp = TunnelServerLaunch.m_InProcess.f_GetActor();
			auto pTunnelServerTrust = TunnelServerLaunch.m_pTrustInterface;
			auto &TunnelServerTrust = *pTunnelServerTrust;
			CStr TunnelServerHostID = TunnelServerLaunch.m_HostID;

			DMibExpect(TunnelServerLaunch.m_HostID, !=, "");

			// Launch Tunnel Client
			CStr TunnelClientDirectory = RootDirectory + "/TunnelClient";
			CFile::fs_CreateDirectory(TunnelClientDirectory);

			auto TunnelClientLaunch = LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "TunnelClient"
					, TunnelClientDirectory
					, []() -> TCActor<CDistributedAppActor>
					{
						return fg_Construct<CTestNetworkTunnelClientApp>();
					}
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
			;

			auto pTunnelClientTrust = TunnelClientLaunch.m_pTrustInterface;
			auto &TunnelClientTrust = *pTunnelClientTrust;
			CStr TunnelClientHostID = TunnelClientLaunch.m_HostID;

			DMibExpect(TunnelClientLaunch.m_HostID, !=, "");

			auto WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;
			auto fPermissions = [&](auto &&_HostID, auto &&_Permissions)
				{
					return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, WaitForSubscriptions};
				}
			;

			auto fNamespaceHosts = [&](auto &&_Namespace, auto &&_Hosts)
				{
					return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, WaitForSubscriptions};
				}
			;

			CDistributedActorTrustManager_Address TunnelServerAddress;
			{
				TunnelServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/Tunnel.sock"_f << TunnelServerDirectory));
				TunnelServerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(TunnelServerAddress).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);
			}
			{
				CDistributedActorTrustManagerInterface::CGenerateConnectionTicket GenerateTicket;
				GenerateTicket.m_Address = TunnelServerAddress;
				auto Ticket = TunnelServerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)(fg_Move(GenerateTicket))
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
				auto HostInfo = TunnelClientTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(fg_Move(Ticket.m_Ticket), g_Timeout, 1)
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;

				TunnelClientTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						fNamespaceHosts(ICNetworkTunnels::mc_pDefaultNamespace, TCSet<CStr>{TunnelServerHostID})
					)
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
				TunnelServerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fPermissions(TunnelClientHostID, TCMap<CStr, CPermissionRequirements>{{"TunnelServerApp/ConnectAll", {}}})
					)
					.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
				;
			}

			auto Tunnels = TunnelClientLaunch.f_Test_Command("EnumTunnels", {}).f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout);

			DMibExpect
				(
					Tunnels
					, ==
					, CEJSONSorted
					{
						"Tunnels"_=
						{
							_(TunnelServerHostID) =
							{
								"TestTunnel"
							}
						}
					}
				)
			;

			CStr TunnelSocketPath = CFile::fs_GetProgramDirectory() / "Sockets/NetworkTunnelGeneralTunnel";

			auto TunnelListenResult = TunnelClientLaunch.f_Test_Command
				(
					"OpenTunnel"
					, {"HostID"_= TunnelServerHostID, "TunnelName"_= "TestTunnel", "ListenHost"_= "UNIX:" + NNetwork::fg_GetSafeUnixSocketPath(TunnelSocketPath / "tests.sock")}
				)
				.f_CallSync(RunLoopHelper.m_pRunLoop, g_Timeout)
			;
		};
	}
};

DMibTestRegister(CNetworkTunnel_Tests, Malterlib::Cloud);
