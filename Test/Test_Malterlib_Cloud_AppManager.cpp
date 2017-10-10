
#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Cloud/VersionManager>
#include <Mib/Cloud/AppManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/App/VersionManager>

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
using namespace NMib::NPtr;
using namespace NMib::NAtomic;

#define DTestAppManagerEnableLogging 0
#define DTestAppManagerEnableOtherOutput 0

static fp64 g_Timeout = 60.0;

class CAppManager_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General")
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
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory + "/AppManagerTests";
			TCSet<CStr> VersionManagerPermissionsForTest = fg_CreateSet<CStr>("Application/WriteAll", "Application/ReadAll", "Application/TagAll"); 

			CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, g_Timeout);
			
			if (CFile::fs_FileExists(RootDirectory))
				CFile::fs_DeleteDirectoryRecursive(RootDirectory);

			CFile::fs_CreateDirectory(RootDirectory);
			
			CTrustManagerTestHelper TrustManagerState;
			TCActor<CDistributedActorTrustManager> TrustManager = TrustManagerState.f_TrustManager("TestHelper");
			CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(g_Timeout);
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			
			CDistributedActorTrustManager_Address ServerAddress;
			ServerAddress.m_URL = fg_Format("wss://[UNIX(777):{}/controller.sock]/", RootDirectory);
			TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(g_Timeout);
			
			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = ServerAddress.m_URL;
			Dependencies.m_TrustManager = TrustManager;
			Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(g_Timeout);
			
			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CVersionManager::mc_pDefaultNamespace);
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CAppManagerInterface::mc_pDefaultNamespace);
			Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(g_Timeout);
			
			TCActor<CDistributedApp_LaunchHelper> LaunchHelper
				= fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestAppManagerEnableLogging || DTestAppManagerEnableOtherOutput)
			;
			auto Cleanup = g_OnScopeExit > [&]
				{
					LaunchHelper->f_BlockDestroy();
				}
			;

			// Launch VersionManager
			CStr VersionManagerDirectory = RootDirectory + "/VersionManager";
			CFile::fs_CreateDirectory(VersionManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/VersionManager", VersionManagerDirectory, nullptr);
			
			auto VersionManagerLaunch = LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchInProcess, "VersionManager", VersionManagerDirectory, &fg_ConstructApp_VersionManager).f_CallSync(g_Timeout);
			
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
					g_Dispatch(FileActor) > [=]
						{
							CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", i);
							CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
							CFile::fs_CreateDirectory(AppManagerDirectory);
							CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/AppManager", AppManagerDirectory, nullptr);
						}
						> AppManagerLaunchesResults.f_AddResult()
					;
				}
				fg_CombineResults(AppManagerLaunchesResults.f_GetResults().f_CallSync());
			}

			// Launch AppManagers
			TCActorResultVector<CDistributedApp_LaunchInfo> AppManagerLaunchesResults;
			
			for (mint i = 0; i < nAppManagers; ++i)
			{
				CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", i);
				CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
				TCVector<CStr> ExtraParams;
#if DTestAppManagerEnableOtherOutput
				if (CFile::fs_GetFile(_Executable) == "AppManager")
					ExtraParams.f_Insert("--log-launches-to-stderr");
#endif
				
				LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchWithParams, AppManagerName, AppManagerDirectory + "/AppManager", fg_Move(ExtraParams)) > AppManagerLaunchesResults.f_AddResult();
			}
			
			TCVector<CDistributedApp_LaunchInfo> AppManagerLaunches;
			for (auto &LaunchResult : AppManagerLaunchesResults.f_GetResults().f_CallSync(g_Timeout))
				AppManagerLaunches.f_Insert(fg_Move(*LaunchResult));

			// Setup VersionMangaer
			auto pVersionManagerTrust = VersionManagerLaunch.m_pTrustInterface;
			auto &VersionManagerTrust = *pVersionManagerTrust;
			CStr VersionManagerHostID = VersionManagerLaunch.m_HostID;

			// Add listen socket that app managers can connect to
			CDistributedActorTrustManager_Address VersionManagerServerAddress;
			VersionManagerServerAddress.m_URL = fg_Format("wss://[UNIX(777):{}/versionmanager.sock]/", VersionManagerDirectory);
			DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddListen, VersionManagerServerAddress).f_CallSync(g_Timeout);
			
			// Add trust to cloud client
			auto Ticket = DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, VersionManagerServerAddress, nullptr).f_CallSync(g_Timeout);
			CStr CloudClientHostID = CProcessLaunch::fs_LaunchTool(CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
			CProcessLaunch::fs_LaunchTool(CloudClientDirectory + "/MalterlibCloud", {"--trust-connection-add", Ticket.m_Ticket.f_ToStringTicket()});
			{
				TCVector<CStr> Params = {"--trust-namespace-add-trusted-host", "--namespace", CVersionManager::mc_pDefaultNamespace, VersionManagerHostID};
				CProcessLaunch::fs_LaunchTool(CloudClientDirectory + "/MalterlibCloud", Params);
				DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddHostPermissions, CloudClientHostID, VersionManagerPermissionsForTest).f_CallSync(g_Timeout);
			}

			// Setup trust between for VersionManager and Test
 			DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_AddHostPermissions, TestHostID, VersionManagerPermissionsForTest).f_CallSync(g_Timeout);
			TrustManager(&CDistributedActorTrustManager::f_AllowHostsForNamespace, CVersionManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(VersionManagerHostID)).f_CallSync(g_Timeout);
			
			auto VersionManager = Subscriptions.f_Subscribe<CVersionManager>();
			CVersionManagerHelper VersionManagerHelper;
			
			auto HelperActor = fg_ConcurrentActor();
			auto fDispatchOnHelper = [&](auto _fToDispatch)
				{
					return
						(
							g_Dispatch > [&]
							{
								return _fToDispatch();
							}
						)
						.f_CallSync(g_Timeout)
					;
				}
			;
			
			CCurrentActorScope CurrentActor{HelperActor};

			// Add initial application to version manager
			CStr TestAppArchive = ProgramDirectory + "/TestApps/TestApp.tar.gz";
			
			auto PackageInfo = fDispatchOnHelper([&]{ return VersionManagerHelper.f_CreatePackage(ProgramDirectory + "/TestApps/TestApp", TestAppArchive); });

			PackageInfo.m_VersionInfo.m_Tags["TestTag"];
			fDispatchOnHelper([&]{return VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);});
			
			// Setup trust for AppManagers
			
			struct CAppManagerInfo
			{
				CStr const &f_GetHostID() const
				{
					return TCMap<CStr, CAppManagerInfo>::fs_GetKey(*this);
				}
				
				TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
				CDistributedActorTrustManager_Address m_Address;
			};

			TCSet<CStr> AllAppManagerHosts;
			TCMap<CStr, CAppManagerInfo> AllAppManagers;
			{
				TCActorResultVector<void> ListenResults;
				mint iAppManager = 0;
				for (auto &AppManager : AppManagerLaunches)
				{
					CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", iAppManager);
					CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
					
					AllAppManagerHosts[AppManager.m_HostID];
					auto &AppManagerInfo = AllAppManagers[AppManager.m_HostID];
					AppManagerInfo.m_pTrustInterface = AppManager.m_pTrustInterface;
					AppManagerInfo.m_Address.m_URL = fg_Format("wss://[UNIX(777):{}/appmanager.sock]/", AppManagerDirectory);
					DMibCallActor(*AppManager.m_pTrustInterface, CDistributedActorTrustManagerInterface::f_AddListen, AppManagerInfo.m_Address) > ListenResults.f_AddResult();
					++iAppManager;
				}
				fg_CombineResults(ListenResults.f_GetResults().f_CallSync(g_Timeout));
			}

			TCActorResultVector<void> SetupTrustResults;
			
			for (auto &AppManager : AllAppManagers)
			{
				auto pAppManagerTrust = AppManager.m_pTrustInterface;
				auto &AppManagerTrust = *pAppManagerTrust; 
				CStr AppManagerHostID = AppManager.f_GetHostID();
				auto TrustAppManagers = AllAppManagerHosts;
				TrustAppManagers.f_Remove(AppManagerHostID);
				DMibCallActor
					(
						AppManagerTrust
						, CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace
						, "com.malterlib/Cloud/AppManagerCoordination"
						, TrustAppManagers
					) 
					> SetupTrustResults.f_AddResult()
				;
				
				DMibCallActor
					(
						TrustManager
						, CDistributedActorTrustManager::f_AllowHostsForNamespace
						, CAppManagerInterface::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(AppManagerHostID)
					)				
					> SetupTrustResults.f_AddResult()
				;
					
				for (auto &AppManagerInner : AllAppManagers)
				{
					CStr AppManagerHostIDInner = AppManagerInner.f_GetHostID();
					if (AppManagerHostIDInner == AppManagerHostID)
						continue;
					
					auto pAppManagerTrustInner = AppManagerInner.m_pTrustInterface; 
					
					TCContinuation<void> Continuation;
					DMibCallActor(AppManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, AppManager.m_Address, nullptr)
						> Continuation / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
						{
							auto &AppManagerTrustInner = *pAppManagerTrustInner;
							DMibCallActor(AppManagerTrustInner, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1) > Continuation.f_ReceiveAny();
						}
					;
					Continuation.f_Dispatch() > SetupTrustResults.f_AddResult();
				}
			}
			
			// Setup trust for version manager
			
			for (auto &AppManager : AllAppManagers)
			{
				auto pAppManagerTrust = AppManager.m_pTrustInterface; 
				auto &AppManagerTrust = *pAppManagerTrust;
				auto &VersionManagerTrust = *pVersionManagerTrust;
				CStr AppManagerHostID = AppManager.f_GetHostID();

				TCContinuation<> Continuation;
				
				DMibCallActor(VersionManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, VersionManagerServerAddress, nullptr)
					+ DMibCallActor
					(
						AppManagerTrust
						, CDistributedActorTrustManagerInterface::f_AddHostPermissions
						, TestHostID
						, fg_CreateSet<CStr>("AppManager/VersionAppAll", "AppManager/CommandAll", "AppManager/AppAll")
					) 
					> Continuation / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket, CVoidTag)
					{
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &VersionManagerTrust = *pVersionManagerTrust;
						DMibCallActor(AppManagerTrust, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1)
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
				Continuation.f_Dispatch() > SetupTrustResults.f_AddResult();
			}
			fg_CombineResults(SetupTrustResults.f_GetResults().f_CallSync(g_Timeout));

			// Install app on app managers
			auto AppManagers = Subscriptions.f_SubscribeMultiple<CAppManagerInterface>(nAppManagers);

			{
				TCActorResultVector<void> AddAppResults;
				for (auto &AppManager : AppManagers)
				{
					CAppManagerInterface::CApplicationAdd Add;
					CAppManagerInterface::CApplicationSettings Settings;
					Settings.m_VersionManagerApplication = "TestApp";
					Settings.m_AutoUpdateTags = fg_CreateSet<CStr>("TestTag");
					Add.m_Version = PackageInfo.m_VersionID;
					
					DMibCallActor(AppManager, CAppManagerInterface::f_Add, "TestApp", Add, Settings) > AddAppResults.f_AddResult();
				}
				fg_CombineResults(AddAppResults.f_GetResults().f_CallSync(g_Timeout));
			}

			// Update Application
			auto fUpdateTestApp = [&]
				{
					++PackageInfo.m_VersionID.m_VersionID.m_Revision;
					fDispatchOnHelper([&]{return VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive);});
				}
			;

			struct CUpdateNotificationsState
			{
				TCVector<CActorSubscription> m_Subscriptions;
				TCSet<mint> m_InProgress;
				mint m_nMaxInProgress = 0;
				TCMap<mint, CAppManagerInterface::EUpdateStage> m_LastInStage;
				TCMap<CAppManagerInterface::EUpdateStage, TCSet<mint>> m_InStage;
				TCMap<CAppManagerInterface::EUpdateStage, zmint> m_MaxInStage;
				TCAtomic<mint> m_nSuccess = 0;
				TCAtomic<mint> m_nFinished = 0;
				NThread::CEventAutoReset m_Event;
				
				void f_Clear()
				{
					m_InProgress.f_Clear();
					m_LastInStage.f_Clear();
					m_InStage.f_Clear();
					m_MaxInStage.f_Clear();
					m_nMaxInProgress = 0;
					m_nSuccess = 0;
					m_nFinished = 0;
				}
			};
			
			TCSharedPointer<CUpdateNotificationsState> pUpdateNotificationsState = fg_Construct();
			
			auto CleanupNotifications = g_OnScopeExit > [&]
				{
					pUpdateNotificationsState->m_Subscriptions.f_Clear();
				}
			;
			
			auto &UpdateNotificationState = *pUpdateNotificationsState;

			// Subscribe for notifications
			{
				TCActorResultVector<void> AppCommandResults;
				mint iAppManager = 0;
				for (auto &AppManager : AppManagers)
				{
					TCContinuation<void> Continuation;
					DMibCallActor
						(
							AppManager
							, CAppManagerInterface::f_SubscribeUpdateNotifications
							, g_ActorFunctor > [pUpdateNotificationsState, iAppManager]
							(CAppManagerInterface::CUpdateNotification const &_Notification) -> TCContinuation<void> 
							{
								auto &State = *pUpdateNotificationsState;
								
								if (auto pInStage = State.m_LastInStage.f_FindEqual(iAppManager))
									State.m_InStage[*pInStage].f_Remove(iAppManager);
								State.m_InStage[_Notification.m_Stage][iAppManager];
								State.m_MaxInStage[_Notification.m_Stage] = fg_Max(State.m_MaxInStage[_Notification.m_Stage], State.m_InStage[_Notification.m_Stage].f_GetLen());
								State.m_LastInStage[iAppManager] = _Notification.m_Stage;
								
								if (_Notification.m_Stage == CAppManagerInterface::EUpdateStage_Failed)
								{
									State.m_InProgress.f_Remove(iAppManager);
									++State.m_nFinished;
								}
								else if (_Notification.m_Stage == CAppManagerInterface::EUpdateStage_Finished)
								{
									State.m_InProgress.f_Remove(iAppManager);
									++State.m_nFinished;
									++State.m_nSuccess;
								}
								else if (_Notification.m_Stage >= CAppManagerInterface::EUpdateStage_StopOldApp)
								{
									State.m_InProgress[iAppManager];
									State.m_nMaxInProgress = fg_Max(State.m_nMaxInProgress, State.m_InProgress.f_GetLen()); 
								}
								State.m_Event.f_Signal();
								return fg_Explicit();
							}
						) 
						> Continuation / [pUpdateNotificationsState, Continuation](NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
						{
							pUpdateNotificationsState->m_Subscriptions.f_Insert(fg_Move(_Subscription));
							Continuation.f_SetResult();
						}
					;
					
					Continuation.f_Dispatch() > AppCommandResults.f_AddResult();
					++iAppManager;
				}
				fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
			}

			auto fWaitForAllUpdated = [&]
				{
					NTime::CClock Clock{true};
					while (true)
					{
						if (pUpdateNotificationsState->m_nFinished == nAppManagers)
							break;
						if (Clock.f_GetTime() > g_Timeout)
							DMibError("Timed out waiting for all apps to update");
						pUpdateNotificationsState->m_Event.f_WaitTimeout(10.0);
					}
				}
			;
			
			auto fSetUpdateType = [&](CStr const &_UpdateType)
				{
					PackageInfo.m_VersionInfo.m_ExtraInfo["ExecutableParameters"] = {"--update-type", _UpdateType, "--daemon-run-standalone"}; 
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_ExecutableParameters = {"--update-type", _UpdateType, "--daemon-run-standalone"};
						
						DMibCallActor(AppManager, CAppManagerInterface::f_ChangeSettings, "TestApp", ChangeSettings, Settings) > AppCommandResults.f_AddResult(); 
					}
					fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
				}
			;

			{
				DMibTestPath("Update Independent");
				fSetUpdateType("Independent");
				UpdateNotificationState.f_Clear();
				fUpdateTestApp();
				fWaitForAllUpdated();
				
				DMibExpect(UpdateNotificationState.m_nSuccess, ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_nMaxInProgress, >= , 1u);
			}
			{
				DMibTestPath("Update OneAtATime");
				fSetUpdateType("OneAtATime");
				UpdateNotificationState.f_Clear();
				fUpdateTestApp();
				fWaitForAllUpdated();

				DMibExpect(UpdateNotificationState.m_nSuccess, ==, nAppManagers);
				DMibTest(DMibExpr(UpdateNotificationState.m_nMaxInProgress) == DMibExpr(1) || DMibExpr(UpdateNotificationState.m_nMaxInProgress) == DMibExpr(2));
				DMibExpect(UpdateNotificationState.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], == , 1);
			}
			{
				DMibTestPath("Update AllAtOnce");
				fSetUpdateType("AllAtOnce");
				UpdateNotificationState.f_Clear();
				fUpdateTestApp();
				fWaitForAllUpdated();
				
				DMibExpect(UpdateNotificationState.m_nSuccess, ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_nMaxInProgress, == , nAppManagers);
				DMibExpect(UpdateNotificationState.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], == , nAppManagers);
			}
		};
	}
};

DMibTestRegister(CAppManager_Tests, Malterlib::Cloud);
