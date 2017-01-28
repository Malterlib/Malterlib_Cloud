
#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorCallbackManager>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Cloud/VersionManager>
#include <Mib/Cryptography/RandomID>

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NFile;
using namespace NMib::NStr;
using namespace NMib::NProcess;
using namespace NMib::NContainer;
using namespace NMib::NCryptography;
using namespace NMib::NCloud;
using namespace NMib::NPtr;

#define DTestAppManagerEnableLogging 0

namespace
{
	struct CLaunchHelperDependencies
	{
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		TCActor<CActorDistributionManager> m_DistributionManager;
		NHTTP::CURL m_Address;
	};

	struct CLaunchInfo
	{
		CLaunchInfo();
		~CLaunchInfo();
		
		CLaunchInfo(CLaunchInfo &&) = default;
		CLaunchInfo(CLaunchInfo const &_Other) = default;
		
		TCActor<CDistributedAppInterfaceLaunchActor> m_Launch;
		TCSharedPointer<CActorSubscription> m_pLaunchSubscription;
		CStr m_HostID;
		CStr m_LaunchID;
		TCSharedPointer<NConcurrency::TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient>> m_pClientInterface;
		TCSharedPointer<NConcurrency::TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
		TCContinuation<CLaunchInfo> m_Continuation;
		
		void f_Abort()
		{
			m_Launch.f_Clear();
			if (m_pClientInterface)
				m_pClientInterface->f_Clear();
			m_pClientInterface.f_Clear();
			if (m_pLaunchSubscription)
				m_pLaunchSubscription->f_Clear();
			m_pLaunchSubscription.f_Clear();
			if (!m_Continuation.f_IsSet())
				m_Continuation.f_SetException(DMibErrorInstance("Aborted"));
		}
	};
	
	CLaunchInfo::CLaunchInfo() = default;
	CLaunchInfo::~CLaunchInfo() = default;
	
	struct CLaunchHelper : public CActor
	{
		struct CDistributedAppInterfaceServerImplementation : public CDistributedAppInterfaceServer
		{
			NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> f_RegisterDistributedApp
				(
					NConcurrency::TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient> &&_ClientInterface
					, NConcurrency::TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface> &&_TrustInterface
					, EDistributedAppUpdateType _UpdateType
				) override
			{
				auto *pThis = m_pThis;
				auto &CallingHostInfo = fg_GetCallingHostInfo();
				
				auto &HostID = CallingHostInfo.f_GetRealHostID();
				
				for (auto &LaunchInfo : pThis->m_Launches)
				{
					if (LaunchInfo.m_HostID != HostID)
						continue;

					LaunchInfo.m_pClientInterface = fg_Construct(fg_Move(_ClientInterface));
					if (_TrustInterface)
						LaunchInfo.m_pTrustInterface = fg_Construct(fg_Move(_TrustInterface));
					if (!LaunchInfo.m_Continuation.f_IsSet())
						LaunchInfo.m_Continuation.f_SetResult(LaunchInfo);
					
					return fg_Explicit(g_ActorSubscription > []{});
				}
				
				return fg_Explicit(nullptr);
			}

			CLaunchHelper *m_pThis = nullptr;
		};
		
		CLaunchHelper(CLaunchHelperDependencies const &_Dependencies)
			: m_Dependencies(_Dependencies)
		{
		}
		
		~CLaunchHelper()
		{
		}
		
		void f_Construct() override
		{
			m_AppInterfaceServer.f_Publish<CDistributedAppInterfaceServer>(m_Dependencies.m_DistributionManager, this, "com.malterlib/Concurrency/DistributedAppInterfaceServer");
		}
		
		TCContinuation<void> f_Destroy() override
		{
			TCActorResultVector<void> Destroys;
			for (auto &Launch : m_Launches)
			{
				Launch.m_Launch->f_Destroy2() > Destroys.f_AddResult();
				Launch.f_Abort();
			}

			m_AppInterfaceServer.f_Destroy() > Destroys.f_AddResult();
			
			TCContinuation<void> Continuation;
			Destroys.f_GetResults() > Continuation.f_ReceiveAny();
			return Continuation;
		}
		
		TCContinuation<CLaunchInfo> f_Launch(CStr const &_Description, CStr const &_Executable)
		{
			CStr LaunchID = fg_RandomID();
			auto &LaunchInfo = m_Launches[LaunchID];
			auto Continuation = LaunchInfo.m_Continuation;
			LaunchInfo.m_LaunchID = LaunchID;
			LaunchInfo.m_Launch = fg_ConstructActor<CDistributedAppInterfaceLaunchActor>
				(
					m_Dependencies.m_Address
					, m_Dependencies.m_TrustManager
					, g_ActorFunctor 
					> [this, LaunchID](NStr::CStr const &_HostID, CCallingHostInfo const &_HostInfo, NContainer::TCVector<uint8> const &_CertificateRequest) -> TCContinuation<void>  
					{
						auto *pLaunch = m_Launches.f_FindEqual(LaunchID);
						DMibCheck(pLaunch);
						pLaunch->m_HostID = _HostID;
						return fg_Explicit();
					}
					, _Description
					, true
				)
			;
			
			auto Params = fg_CreateVector<CStr>("--daemon-run");
			
#if DTestAppManagerEnableLogging
			Params.f_Insert("--log-to-stderr");
#endif
			
			CProcessLaunchActor::CLaunch Launch
				{
					CProcessLaunchParams::fs_LaunchExecutable
					(
						_Executable
						, fg_Move(Params)
						, CFile::fs_GetPath(_Executable)
						, [this, Continuation](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
						{
							switch (_State.f_GetTypeID())
							{
							case EProcessLaunchState_LaunchFailed:
								{
									if (!Continuation.f_IsSet())
										Continuation.f_SetException(DMibErrorInstance(fg_Format("Launch failed: {}", _State.f_Get<EProcessLaunchState_LaunchFailed>())));
									break;
								}
							case EProcessLaunchState_Exited:
								{
									if (!Continuation.f_IsSet())
										Continuation.f_SetException(DMibErrorInstance(fg_Format("Launch exited unexpectedly: {}", _State.f_Get<EProcessLaunchState_Exited>())));
									break;
								}
							case EProcessLaunchState_Launched:
								break;
							}
						}
					)
				}
			;
			
#if DTestAppManagerEnableLogging
			Launch.m_ToLog = CProcessLaunchActor::ELogFlag_All;
#endif
			
			LaunchInfo.m_Launch(&CProcessLaunchActor::f_Launch, Launch, fg_ThisActor(this)) > Continuation / [this, LaunchID](NConcurrency::CActorSubscription &&_Subscription)
				{
					auto *pLaunch = m_Launches.f_FindEqual(LaunchID);
					if (!pLaunch)
						return;
					pLaunch->m_pLaunchSubscription = fg_Construct(fg_Move(_Subscription));
				}
			;
			
			return Continuation;
		}

		CLaunchHelperDependencies m_Dependencies;
		TCMap<CStr, CLaunchInfo> m_Launches;
		TCDelegatedActorInterface<CDistributedAppInterfaceServerImplementation> m_AppInterfaceServer;
	};
}

class CAppManager_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory + "/AppManagerTests";
			TCSet<CStr> VersionManagerPermissionsForTest = fg_CreateSet<CStr>("Application/WriteAll", "Application/ReadAll", "Application/TagAll"); 

			CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, 30.0);
			
			if (CFile::fs_FileExists(RootDirectory))
				CFile::fs_DeleteDirectoryRecursive(RootDirectory);

			CFile::fs_CreateDirectory(RootDirectory);
			
			CTrustManagerTestHelper TrustManagerState;
			TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
			CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(60.0);
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			
			CDistributedActorTrustManager_Address ServerAddress;
			ServerAddress.m_URL = fg_Format("wss://[UNIX(777):{}/controller.sock]/", RootDirectory);
			TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(60.0);
			
			CLaunchHelperDependencies Dependencies;
			Dependencies.m_Address = ServerAddress.m_URL;
			Dependencies.m_TrustManager = TrustManager;
			Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(60.0);
			
			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace); 
			Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(60.0);
			
			TCActor<CLaunchHelper> LaunchHelper = fg_ConstructActor<CLaunchHelper>(Dependencies);
			auto Cleanup = g_OnScopeExit > [&]
				{
					LaunchHelper->f_BlockDestroy();
				}
			;

			// Launch VersionManager
			CStr VersionManagerDirectory = RootDirectory + "/VersionManager";
			CFile::fs_CreateDirectory(VersionManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/VersionManager", VersionManagerDirectory, nullptr);
			
			auto VersionManagerLaunch = LaunchHelper(&CLaunchHelper::f_Launch, "VersionManager", VersionManagerDirectory + "/VersionManager").f_CallSync(30.0);
			
			DMibExpect(VersionManagerLaunch.m_HostID, !=, "");

			// Copy Cloud Client for debugging
			CStr CloudClientDirectory = RootDirectory + "/MalterlibCloud";
			CFile::fs_CreateDirectory(CloudClientDirectory);
			CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/MalterlibCloud", CloudClientDirectory, nullptr);
			
			// Copy AppManagers to their directories
			mint nAppManagers = 10;
			{
				TCActorResultVector<void> AppManagerLaunchesResults;
				TCVector<TCActor<CSeparateThreadActor>> FileActors;
				for (mint i = 0; i < nAppManagers; ++i)
				{
					auto &FileActor = FileActors.f_Insert() = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
					fg_Dispatch
						(
							FileActor
							, [=]
							{
								CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", i);
								CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
								CFile::fs_CreateDirectory(AppManagerDirectory);
								CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/AppManager", AppManagerDirectory, nullptr);
							}
						)
						> AppManagerLaunchesResults.f_AddResult()
					;
				}
				fg_CombineResults(AppManagerLaunchesResults.f_GetResults().f_CallSync());
			}

			// Launch AppManagers
			TCActorResultVector<CLaunchInfo> AppManagerLaunchesResults;
			
			for (mint i = 0; i < nAppManagers; ++i)
			{
				CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", i);
				CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
				LaunchHelper(&CLaunchHelper::f_Launch, AppManagerName, AppManagerDirectory + "/AppManager") > AppManagerLaunchesResults.f_AddResult();
			}
			
			TCVector<CLaunchInfo> AppManagerLaunches;
			for (auto &LaunchResult : AppManagerLaunchesResults.f_GetResults().f_CallSync(30.0))
				AppManagerLaunches.f_Insert(fg_Move(*LaunchResult));

			// Setup VersionMangaer
			auto pVersionManagerTrust = VersionManagerLaunch.m_pTrustInterface;
			auto &VersionManagerTrust = *pVersionManagerTrust;
			CStr VersionManagerHostID = VersionManagerLaunch.m_HostID;

			// Add listen socket that app managers can connect to
			CDistributedActorTrustManager_Address VersionManagerServerAddress;
			VersionManagerServerAddress.m_URL = fg_Format("wss://[UNIX(777):{}/versionmanager.sock]/", VersionManagerDirectory);
			DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddListen, VersionManagerServerAddress).f_CallSync(60.0);
			
			// Add trust to cloud client
			auto Ticket = DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, VersionManagerServerAddress, nullptr).f_CallSync(60.0);
			CStr CloudClientHostID;
			{
				DMibTestPath("Cloud Client Get Host ID");
				CStr StdErr;
				uint32 ExitCode = 0;
				CProcessLaunch::fs_LaunchBlock(CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id"), CloudClientHostID, StdErr, ExitCode);
				DMibAssert(ExitCode, ==, 0);
				CloudClientHostID = CloudClientHostID.f_Trim();
			}
			{
				DMibTestPath("Cloud Client Add Connection");
				CStr StdErr;
				CStr StdOut;
				uint32 ExitCode = 0;
				CProcessLaunch::fs_LaunchBlock(CloudClientDirectory + "/MalterlibCloud", {"--trust-connection-add", Ticket.m_Ticket.f_ToStringTicket()}, StdOut, StdErr, ExitCode);
				DMibAssert(ExitCode, ==, 0);
			}
			{
				DMibTestPath("Cloud Client Trust");
				CStr StdErr;
				CStr StdOut;
				uint32 ExitCode = 0;
				TCVector<CStr> Params = {"--trust-namespace-add-trusted-host", "--namespace", CVersionManager::mc_pDefaultNamespace, VersionManagerHostID};
				CProcessLaunch::fs_LaunchBlock(CloudClientDirectory + "/MalterlibCloud", Params, StdOut, StdErr, ExitCode);
				DMibAssert(ExitCode, ==, 0);
				
				DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddHostPermissions, CloudClientHostID, VersionManagerPermissionsForTest).f_CallSync(60.0);
			}

			// Setup trust between for VersionManager and Test
 			DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddHostPermissions, TestHostID, VersionManagerPermissionsForTest).f_CallSync(60.0);
			TrustManager(&CDistributedActorTrustManager::f_AllowHostsForNamespace, CVersionManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(VersionManagerHostID)).f_CallSync(60.0);
			
			auto VersionManager = Subscriptions.f_Subscribe<CVersionManager>();
			CVersionManagerHelper VersionManagerHelper;

			// Add initial application to version manager
			auto PackageInfo = fg_ConcurrentDispatch
				(
					[=]() mutable
					{
						return VersionManagerHelper.f_CreatePackage(ProgramDirectory + "/TestApps/TestApp", ProgramDirectory + "/TestApps/TestApp.tar.gz");
					}
				).f_CallSync(60.0)
			;
			fg_ConcurrentDispatch
				(
					[=]
					{
						return VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, ProgramDirectory + "/TestApps/TestApp.tar.gz");
					}
				).f_CallSync(60.0)
			;
			
			// Setup trust for AppManagers
			TCActorResultVector<void> SetupVersionManagerResults;
			
			for (auto &AppManager : AppManagerLaunches)
			{
				fg_ConcurrentDispatch
					(
						[=, pAppManagerTrust = AppManager.m_pTrustInterface]
						{
							auto &AppManagerTrust = *pAppManagerTrust;
							auto &VersionManagerTrust = *pVersionManagerTrust;
							TCContinuation<> Continuation;
							DMibCallActor(AppManagerTrust, CDistributedActorTrustManagerInterface::f_GetHostID)
								+ DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, VersionManagerServerAddress, nullptr)
								> Continuation / [=]
								(CStr &&_HostID, CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
								{
									auto &AppManagerTrust = *pAppManagerTrust;
									auto &VersionManagerTrust = *pVersionManagerTrust;
									CStr AppManagerHostID = _HostID;
									DMibCallActor(AppManagerTrust, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, 30.0)
										+ DMibCallActor
										(
											VersionManagerTrust
											, CDistributedActorTrustManagerInterface::f_AddHostPermissions
											, AppManagerHostID
											, fg_CreateSet<CStr>("Application/ReadAll")
										)
										+ DMibCallActor
										(
											AppManagerTrust
											, CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace
											, CVersionManager::mc_pDefaultNamespace
											, fg_CreateSet<CStr>(VersionManagerHostID)
										)
										> Continuation / [=]()
										{
											Continuation.f_SetResult();
										}
									;
								}
							;
							return Continuation;
						}
					)
					> SetupVersionManagerResults.f_AddResult()
				;
			}
			fg_CombineResults(SetupVersionManagerResults.f_GetResults().f_CallSync(60.0));
		};
	}
};

DMibTestRegister(CAppManager_Tests, Malterlib::Cloud);
