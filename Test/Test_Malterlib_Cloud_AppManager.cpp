
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
#include <Mib/Cloud/CloudManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/App/VersionManager>
#include <Mib/Cloud/App/AppManager>
#include <Mib/Cloud/App/CloudManager>

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
using namespace NMib::NEncoding;
using namespace NMib::NAtomic;
using namespace NMib::NNetwork;

#define DTestAppManagerEnableLogging 0
#define DTestAppManagerEnableOtherOutput 0

static fp64 g_Timeout = 60.0;
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
			fg_GetSys()->f_SetEnvironmentVariable("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
			NSys::fg_Process_SetEnvironmentVariable_Unsafe("Path", "c:\\Program Files\\Git\\usr\\bin;{}"_f << fg_GetSys()->f_GetEnvironmentVariable("Path"));
#endif
#if DTestAppManagerEnableLogging
			fg_GetSys()->f_AddStdErrLogger();
#endif

			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory + "/AppManagerTests";
			auto VersionManagerPermissionsForTest = fg_CreateMap<CStr, CPermissionRequirements>("Application/WriteAll", "Application/ReadAll", "Application/TagAll");
			auto CloudManagerPermissionsForTest = fg_CreateMap<CStr, CPermissionRequirements>("CloudManager/ReadAll");

			CProcessLaunch::fs_KillProcessesInDirectory("*", {}, RootDirectory, g_Timeout);

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
			auto CleanupTrustManager = g_OnScopeExit > [&]
				{
					TrustManager->f_BlockDestroy();
				}
			;

			CStr TestHostID = TrustManager(&CDistributedActorTrustManager::f_GetHostID).f_CallSync(g_Timeout);
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};

			CDistributedActorTrustManager_Address ServerAddress;

			ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/controller.sock"_f << RootDirectory);
			TrustManager(&CDistributedActorTrustManager::f_AddListen, ServerAddress).f_CallSync(g_Timeout);

			CDistributedApp_LaunchHelperDependencies Dependencies;
			Dependencies.m_Address = ServerAddress.m_URL;
			Dependencies.m_TrustManager = TrustManager;
			Dependencies.m_DistributionManager = TrustManager(&CDistributedActorTrustManager::f_GetDistributionManager).f_CallSync(g_Timeout);

			NMib::NConcurrency::CDistributedActorSecurity Security;
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CCloudManager::mc_pDefaultNamespace);
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

			auto VersionManagerLaunch = LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager"
					, VersionManagerDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(g_Timeout)
			;

			DMibExpect(VersionManagerLaunch.m_HostID, !=, "");

			// Launch CloudManager
			CStr CloudManagerDirectory = RootDirectory + "/CloudManager";
			CFile::fs_CreateDirectory(CloudManagerDirectory);
			CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/CloudManager", CloudManagerDirectory, nullptr);

			auto CloudManagerLaunch = LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "CloudManager"
					, CloudManagerDirectory
					, &fg_ConstructApp_CloudManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_CallSync(g_Timeout)
			;

			DMibExpect(CloudManagerLaunch.m_HostID, !=, "");

			// Copy Cloud Client for debugging
			CStr CloudClientDirectory = RootDirectory + "/MalterlibCloud";
			CFile::fs_CreateDirectory(CloudClientDirectory);
			CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/MalterlibCloud", CloudClientDirectory, nullptr);

			// Copy AppManagers to their directories
			mint nAppManagers = 10;
#if DMibPPtrBits <= 32
			nAppManagers = 2;
#endif
			{
				TCActorResultVector<void> AppManagerLaunchesResults;
				TCVector<TCActor<CSeparateThreadActor>> FileActors;
				for (mint i = 0; i < nAppManagers; ++i)
				{
					auto &FileActor = FileActors.f_Insert() = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
					g_Dispatch(FileActor) / [=]
						{
							CStr AppManagerName = fg_Format("AppManager{sf0,sl2}", i);
							CStr AppManagerDirectory = RootDirectory + "/" + AppManagerName;
							CFile::fs_CreateDirectory(AppManagerDirectory);
							CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/AppManager", AppManagerDirectory, nullptr);
						}
						> AppManagerLaunchesResults.f_AddResult()
					;
				}
				DMibTestMark;
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
				ExtraParams.f_Insert("--log-launches-to-stderr");
#endif

#if 0
				LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchWithParams, AppManagerName, AppManagerDirectory + "/AppManager", fg_Move(ExtraParams))
					> AppManagerLaunchesResults.f_AddResult()
				;
#else
				LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchInProcess, AppManagerName, AppManagerDirectory, &fg_ConstructApp_AppManager, fg_Move(ExtraParams))
					> AppManagerLaunchesResults.f_AddResult()
				;
#endif
			}

			TCVector<CDistributedApp_LaunchInfo> AppManagerLaunches;
			DMibTestMark;
			for (auto &LaunchResult : AppManagerLaunchesResults.f_GetResults().f_CallSync(g_Timeout))
				AppManagerLaunches.f_Insert(fg_Move(*LaunchResult));

			// Setup VersionMangaer
			auto pVersionManagerTrust = VersionManagerLaunch.m_pTrustInterface;
			auto &VersionManagerTrust = *pVersionManagerTrust;
			CStr VersionManagerHostID = VersionManagerLaunch.m_HostID;

			// Add listen socket that app managers can connect to
			CDistributedActorTrustManager_Address VersionManagerServerAddress;

			VersionManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/versionmanager.sock"_f << VersionManagerDirectory));
			DMibTestMark;
			VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(VersionManagerServerAddress).f_CallSync(g_Timeout);

			// Setup CloudMangaer
			auto pCloudManagerTrust = CloudManagerLaunch.m_pTrustInterface;
			auto &CloudManagerTrust = *pCloudManagerTrust;
			CStr CloudManagerHostID = CloudManagerLaunch.m_HostID;

			// Add listen socket that app managers can connect to
			CDistributedActorTrustManager_Address CloudManagerServerAddress;

			CloudManagerServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/cloudmanager.sock"_f << CloudManagerDirectory));
			DMibTestMark;
			CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(CloudManagerServerAddress).f_CallSync(g_Timeout);

			// Permission helpers

			static auto constexpr c_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;
			auto fPermissions = [](auto &&_HostID, auto &&_Permissions)
				{
					return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, c_WaitForSubscriptions};
				}
			;
			auto fNamespaceHosts = [](auto &&_Namespace, auto &&_Hosts)
				{
					return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, c_WaitForSubscriptions};
				}
			;

			// Add trust to cloud client
			DMibTestMark;
			{
				CStr CloudClientHostID = CProcessLaunch::fs_LaunchTool(CloudClientDirectory + "/MalterlibCloud", fg_CreateVector<CStr>("--trust-host-id")).f_Trim();
				{
					auto Ticket = VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
						(
							CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{VersionManagerServerAddress}
						)
						.f_CallSync(g_Timeout)
					;
					CProcessLaunch::fs_LaunchTool
						(
						 	CloudClientDirectory + "/MalterlibCloud"
						 	, {"--trust-connection-add", "--trusted-namespaces", CJSON{CVersionManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
						)
					;
					DMibTestMark;
					VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissions(CloudClientHostID, VersionManagerPermissionsForTest)
						)
						.f_CallSync(g_Timeout)
					;
				}
				{
					auto Ticket = CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
						(
							CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{CloudManagerServerAddress}
						)
						.f_CallSync(g_Timeout)
					;
					CProcessLaunch::fs_LaunchTool
						(
						 	CloudClientDirectory + "/MalterlibCloud"
						 	, {"--trust-connection-add", "--trusted-namespaces", CJSON{CCloudManager::mc_pDefaultNamespace}.f_ToString(nullptr), Ticket.m_Ticket.f_ToStringTicket()}
						)
					;
					DMibTestMark;
					CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							fPermissions(CloudClientHostID, CloudManagerPermissionsForTest)
						)
						.f_CallSync(g_Timeout)
					;
				}
			}

			// Setup trust between VersionManager and Test
			DMibTestMark;
 			VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fPermissions(TestHostID, VersionManagerPermissionsForTest)
				)
				.f_CallSync(g_Timeout)
			;
			DMibTestMark;
			TrustManager
				(
					 &CDistributedActorTrustManager::f_AllowHostsForNamespace
					 , CVersionManager::mc_pDefaultNamespace
					 , fg_CreateSet<CStr>(VersionManagerHostID)
					 , c_WaitForSubscriptions
				)
				.f_CallSync(g_Timeout)
			;

			// Setup trust between CloudManager and Test
			DMibTestMark;
 			CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
				(
					 fPermissions(TestHostID, CloudManagerPermissionsForTest)
				)
				.f_CallSync(g_Timeout)
			;
			DMibTestMark;
			TrustManager
				(
					 &CDistributedActorTrustManager::f_AllowHostsForNamespace
					 , CCloudManager::mc_pDefaultNamespace
					 , fg_CreateSet<CStr>(CloudManagerHostID)
					 , c_WaitForSubscriptions
				)
				.f_CallSync(g_Timeout)
			;

			TCActor<CSeparateThreadActor> HelperActor{fg_Construct(), "Test actor"};
			auto CleanupTestActor = g_OnScopeExit > [&]
				{
					HelperActor->f_BlockDestroy();
				}
			;
			CCurrentActorScope CurrentActor{HelperActor};

			auto CloudManager = Subscriptions.f_Subscribe<CCloudManager>();

			auto VersionManager = Subscriptions.f_Subscribe<CVersionManager>();
			CVersionManagerHelper VersionManagerHelper(VersionManagerDirectory);

			// Add initial application to version manager
			CStr TestAppArchive = ProgramDirectory + "/TestApps/TestApp.tar.gz";

			DMibTestMark;
			auto PackageInfo = VersionManagerHelper.f_CreatePackage(ProgramDirectory + "/TestApps/TestApp", TestAppArchive, g_CompressionLevel).f_CallSync(g_Timeout);

			PackageInfo.m_VersionInfo.m_Tags["TestTag"];
			DMibTestMark;
			VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive).f_CallSync(g_Timeout);

			// Setup trust for AppManagers

			struct CAppManagerInfo
			{
				CStr const &f_GetHostID() const
				{
					return TCMap<CStr, CAppManagerInfo>::fs_GetKey(*this);
				}

				TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
				CDistributedActorTrustManager_Address m_Address;
				CStr m_RootDirectory;
				CStr m_HostID;
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
					AppManagerInfo.m_Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/appmanager.sock"_f << AppManagerDirectory);
					AppManagerInfo.m_RootDirectory = AppManagerDirectory;
					AppManagerInfo.m_HostID = AppManager.m_HostID;
					AppManager.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(AppManagerInfo.m_Address) > ListenResults.f_AddResult();
					++iAppManager;
				}
				DMibTestMark;
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
				AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						fNamespaceHosts("com.malterlib/Cloud/AppManagerCoordination", TrustAppManagers)
					)
					> SetupTrustResults.f_AddResult()
				;

				TrustManager.f_CallActor(&CDistributedActorTrustManager::f_AllowHostsForNamespace)
					(
						CAppManagerInterface::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(AppManagerHostID)
						, c_WaitForSubscriptions
					)
					> SetupTrustResults.f_AddResult()
				;

				for (auto &AppManagerInner : AllAppManagers)
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
							AppManagerTrustInner.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
						}
					;
					Promise.f_Dispatch() > SetupTrustResults.f_AddResult();
				}
			}

			// Setup trust for version manager

			for (auto &AppManager : AllAppManagers)
			{
				auto pAppManagerTrust = AppManager.m_pTrustInterface;
				auto &AppManagerTrust = *pAppManagerTrust;
				auto &VersionManagerTrust = *pVersionManagerTrust;
				auto &CloudManagerTrust = *pCloudManagerTrust;
				CStr AppManagerHostID = AppManager.f_GetHostID();

				TCPromise<> Promise;

				VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
					 	CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{VersionManagerServerAddress}
					)
					+ CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
					 	CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{CloudManagerServerAddress}
					)
					+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						fPermissions(TestHostID, fg_CreateMap<CStr, CPermissionRequirements>("AppManager/VersionAppAll", "AppManager/CommandAll", "AppManager/AppAll"))
					)
					> Promise / [=]
					(
					 	CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_VersionManagerTicket
					 	, CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_CloudManagerTicket
					 	, CVoidTag
					)
					{
						auto &AppManagerTrust = *pAppManagerTrust;
						auto &VersionManagerTrust = *pVersionManagerTrust;
						auto &CloudManagerTrust = *pCloudManagerTrust;
						AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_VersionManagerTicket.m_Ticket, g_Timeout, -1)
							+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)(_CloudManagerTicket.m_Ticket, g_Timeout, -1)
							+ VersionManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fPermissions(AppManagerHostID, fg_CreateMap<CStr, CPermissionRequirements>("Application/ReadAll"))
							)
							+ CloudManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
							(
								fPermissions(AppManagerHostID, fg_CreateMap<CStr, CPermissionRequirements>("CloudManager/RegisterAppManager"))
							)
							+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(CVersionManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(VersionManagerHostID))
							)
							+ AppManagerTrust.f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
							(
								fNamespaceHosts(CCloudManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(CloudManagerHostID))
							)
							> Promise / [=]()
							{
								Promise.f_SetResult();
							}
						;
					}
				;
				Promise.f_Dispatch() > SetupTrustResults.f_AddResult();
			}
			DMibTestMark;
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
					Settings.m_UpdateGroup = "TestGroup";
					Add.m_Version = PackageInfo.m_VersionID;

					AppManager.f_CallActor(&CAppManagerInterface::f_Add)("TestApp", Add, Settings) > AddAppResults.f_AddResult();
				}
				DMibTestMark;
				fg_CombineResults(AddAppResults.f_GetResults().f_CallSync(g_Timeout));
			}

			// Update Application
 			auto fUpdateTestApp = [&](TCSet<CStr> const &_Tags)
				{
					++PackageInfo.m_VersionID.m_VersionID.m_Revision;
					PackageInfo.m_VersionInfo.m_Tags = _Tags;
					VersionManagerHelper.f_Upload(VersionManager, "TestApp", PackageInfo.m_VersionID, PackageInfo.m_VersionInfo, TestAppArchive).f_CallSync(g_Timeout);
				}
			;

			auto fTagApp = [&](CStr const &_Name, TCSet<CStr> const &_Tags)
				{
					CVersionManager::CChangeTags ChangeTags;
					ChangeTags.m_AddTags = _Tags;
					ChangeTags.m_Application = _Name;
					ChangeTags.m_VersionID = PackageInfo.m_VersionID.m_VersionID;
					VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(fg_Move(ChangeTags)).f_CallSync(g_Timeout);
				}
			;

			struct CApplicationKey
			{
				auto f_Tuple() const
				{
					return fg_TupleReferences(m_AppName, m_iAppManager);
				}

				bool operator < (CApplicationKey const &_Right) const
				{
					return f_Tuple() < _Right.f_Tuple();
				}

				CStr m_AppName;
				mint m_iAppManager;
			};

			struct CUpdateNotificationsApplicationState
			{
				TCSet<CApplicationKey> m_InProgress;
				mint m_nMaxInProgress = 0;

				TCMap<CApplicationKey, CAppManagerInterface::EUpdateStage> m_LastInStage;
				TCMap<CApplicationKey, CAppManagerInterface::EUpdateStage> m_LastInStageCoordination;
				TCMap<CAppManagerInterface::EUpdateStage, TCSet<CApplicationKey>> m_InStage;
				TCMap<CAppManagerInterface::EUpdateStage, TCSet<CApplicationKey>> m_InStageCoordination;

				TCMap<CAppManagerInterface::EUpdateStage, zmint> m_MaxInStage;
				TCMap<CAppManagerInterface::EUpdateStage, zmint> m_MaxInStageCoordination;
				TCAtomic<mint> m_nSuccess = 0;
				TCAtomic<mint> m_nFinished = 0;

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

			struct CUpdateNotificationsState
			{
				void f_Destroy()
				{
					TCActorResultVector<void> Destroys;
					for (auto &Subscription : m_Subscriptions)
						Subscription->f_Destroy() > Destroys.f_AddResult();;
					Destroys.f_GetResults().f_CallSync();
					f_Clear();
				}

				void f_Clear()
				{
					for (auto &Application : m_Applications)
						Application.f_Clear();
					for (auto &AppManager : m_ApplicationsPerAppmanager)
					{
						for (auto &Application : AppManager)
							Application.f_Clear();
					}

					m_AllApplications.f_Clear();
					m_AppsInProgressPerAppManager.f_Clear();
					m_MaxAppsInProgressPerAppManager.f_Clear();
					m_nAppsInProgress = 0;
					m_nMaxAppsInProgress = 0;
					m_nMaxAppsInProgressPerAppManager = 0;
				}

				TCVector<CActorSubscription> m_Subscriptions;
				NThread::CEventAutoReset m_Event;
				TCMap<CStr, CUpdateNotificationsApplicationState> m_Applications;
				TCMap<mint, TCMap<CStr, CUpdateNotificationsApplicationState>> m_ApplicationsPerAppmanager;
				CUpdateNotificationsApplicationState m_AllApplications;
				mint m_nAppsInProgress = 0;
				mint m_nMaxAppsInProgress = 0;
				TCMap<mint, zmint> m_AppsInProgressPerAppManager;
				TCMap<mint, zmint> m_MaxAppsInProgressPerAppManager;
				mint m_nMaxAppsInProgressPerAppManager = 0;
			};

			TCSharedPointer<CUpdateNotificationsState> pUpdateNotificationsState = fg_Construct();

			pUpdateNotificationsState->m_Applications["TestApp"];
			pUpdateNotificationsState->m_Applications["TestApp2"];

			auto CleanupNotifications = g_OnScopeExit > [&]
				{
					pUpdateNotificationsState->f_Destroy();
				}
			;

			auto &UpdateNotificationState = *pUpdateNotificationsState;

			TCSharedPointer<CStr> pUpdateType = fg_Construct();

			// Subscribe for notifications
			{
				TCActorResultVector<void> AppCommandResults;
				mint iAppManager = 0;
				for (auto &AppManager : AppManagers)
				{
					TCPromise<void> Promise;
					AppManager.f_CallActor(&CAppManagerInterface::f_SubscribeUpdateNotifications)
						(
							g_ActorFunctor / [pUpdateNotificationsState, iAppManager]
							(CAppManagerInterface::CUpdateNotification const &_Notification) -> TCFuture<void>
							{
								CApplicationKey ApplicationKey{_Notification.m_Application, iAppManager};
								auto &WholeState = *pUpdateNotificationsState;
								auto fProcessApplicationState = [&](CUpdateNotificationsApplicationState &_State)
									{
										if (!_Notification.m_bCoordinateWait)
										{
											if (auto pInStage = _State.m_LastInStage.f_FindEqual(ApplicationKey))
												_State.m_InStage[*pInStage].f_Remove(ApplicationKey);
											_State.m_InStage[_Notification.m_Stage][ApplicationKey];
											_State.m_MaxInStage[_Notification.m_Stage] = fg_Max
												(
													_State.m_MaxInStage[_Notification.m_Stage]
												 	, _State.m_InStage[_Notification.m_Stage].f_GetLen()
												)
											;
											_State.m_LastInStage[ApplicationKey] = _Notification.m_Stage;
										}

										if (auto pInStage = _State.m_LastInStageCoordination.f_FindEqual(ApplicationKey))
											_State.m_InStageCoordination[*pInStage].f_Remove(ApplicationKey);
										_State.m_InStageCoordination[_Notification.m_Stage][ApplicationKey];
										_State.m_MaxInStageCoordination[_Notification.m_Stage] = fg_Max
											(
											 	_State.m_MaxInStageCoordination[_Notification.m_Stage]
											 	, _State.m_InStageCoordination[_Notification.m_Stage].f_GetLen()
											)
										;
										_State.m_LastInStageCoordination[ApplicationKey] = _Notification.m_Stage;

										if (_Notification.m_Stage == CAppManagerInterface::EUpdateStage_Failed)
										{
											_State.m_InProgress.f_Remove(ApplicationKey);
											++_State.m_nFinished;
										}
										else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage == CAppManagerInterface::EUpdateStage_Finished)
										{
											_State.m_InProgress.f_Remove(ApplicationKey);
											++_State.m_nFinished;
											++_State.m_nSuccess;
										}
										else if (!_Notification.m_bCoordinateWait && _Notification.m_Stage >= CAppManagerInterface::EUpdateStage_StopOldApp)
										{
											_State.m_InProgress[ApplicationKey];
											_State.m_nMaxInProgress = fg_Max(_State.m_nMaxInProgress, _State.m_InProgress.f_GetLen());
										}
									}
								;

								fProcessApplicationState(WholeState.m_Applications[_Notification.m_Application]);
								fProcessApplicationState(WholeState.m_ApplicationsPerAppmanager[iAppManager][_Notification.m_Application]);
								fProcessApplicationState(WholeState.m_AllApplications);
								//DMibConOut2("{} {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_ApplicationsPerAppmanager[iAppManager][_Notification.m_Application].m_MaxInStage);
								//DMibConOut2("{} N {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_Applications[_Notification.m_Application].m_MaxInStage);
								//DMibConOut2("{} C {a-,sj9} {} {vs}\n", iAppManager, _Notification.m_Application, _Notification.m_Stage, WholeState.m_Applications[_Notification.m_Application].m_MaxInStageCoordination);
								//DMibConOut2("{a-,sj9} {} {vs}\n", _Notification.m_Application, CAppManagerInterface::EUpdateStage_StopOldApp, WholeState.m_AllApplications.m_MaxInStage);

								WholeState.m_nAppsInProgress = 0;
								for (auto &App : WholeState.m_Applications)
								{
									if (!App.m_InProgress.f_IsEmpty())
										++WholeState.m_nAppsInProgress;
								}
								WholeState.m_nMaxAppsInProgress = fg_Max(WholeState.m_nAppsInProgress, WholeState.m_nMaxAppsInProgress);

								WholeState.m_nMaxAppsInProgressPerAppManager = 0;
								for (auto &AppManager : WholeState.m_ApplicationsPerAppmanager)
								{
									auto &iAppManager = WholeState.m_ApplicationsPerAppmanager.fs_GetKey(AppManager);
									WholeState.m_AppsInProgressPerAppManager[iAppManager] = 0;
									for (auto &App : AppManager)
									{
										if (!App.m_InProgress.f_IsEmpty())
											++WholeState.m_AppsInProgressPerAppManager[iAppManager];
									}
									WholeState.m_MaxAppsInProgressPerAppManager[iAppManager] = fg_Max
										(
										 	WholeState.m_AppsInProgressPerAppManager[iAppManager]
										 	, WholeState.m_MaxAppsInProgressPerAppManager[iAppManager]
										)
									;
									WholeState.m_nMaxAppsInProgressPerAppManager = fg_Max
										(
										 	WholeState.m_nMaxAppsInProgressPerAppManager
										 	, WholeState.m_MaxAppsInProgressPerAppManager[iAppManager]
										)
									;
								}

								WholeState.m_Event.f_Signal();
								return fg_Explicit();
							}
						)
						> Promise / [pUpdateNotificationsState, Promise](NConcurrency::TCActorSubscriptionWithID<> &&_Subscription)
						{
							pUpdateNotificationsState->m_Subscriptions.f_Insert(fg_Move(_Subscription));
							Promise.f_SetResult();
						}
					;

					Promise.f_Dispatch() > AppCommandResults.f_AddResult();
					++iAppManager;
				}
				DMibTestMark;
				fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
			}

			auto fWaitForAllUpdated = [&](CStr const &_Application)
				{
					auto &ApplicationState = pUpdateNotificationsState->m_Applications[_Application];
					NTime::CClock Clock{true};
					while (true)
					{
						if (ApplicationState.m_nFinished == nAppManagers)
							break;
						if (Clock.f_GetTime() > g_Timeout)
							DMibError("Timed out waiting for all apps to update");
						pUpdateNotificationsState->m_Event.f_WaitTimeout(10.0);
					}
				}
			;

			auto fSetUpdateType = [&](CStr const &_AppName, CStr const &_UpdateType)
				{
					PackageInfo.m_VersionInfo.m_ExtraInfo["ExecutableParameters"] = {"--update-type", _UpdateType, "--daemon-run-standalone"};
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_ExecutableParameters = {"--update-type", _UpdateType, "--daemon-run-standalone"};

						AppManager.f_CallActor(&CAppManagerInterface::f_ChangeSettings)(_AppName, ChangeSettings, Settings) > AppCommandResults.f_AddResult();
					}
					fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
					*pUpdateType = _UpdateType;
				}
			;

			auto fSetUpdateGroup = [&](CStr const &_AppName, CStr const &_Group)
				{
					TCActorResultVector<void> AppCommandResults;
					for (auto &AppManager : AppManagers)
					{
						CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
						CAppManagerInterface::CApplicationSettings Settings;
						Settings.m_UpdateGroup = _Group;

						AppManager.f_CallActor(&CAppManagerInterface::f_ChangeSettings)(_AppName, ChangeSettings, Settings) > AppCommandResults.f_AddResult();
					}
					fg_CombineResults(AppCommandResults.f_GetResults().f_CallSync(g_Timeout));
				}
			;

			{
				DMibTestPath("CloudManager");

				auto AppManagers = CloudManager.f_CallActor(&CCloudManager::f_EnumAppManagers)().f_CallSync(g_Timeout);
				DMibExpect(AppManagers.f_GetLen(), ==, nAppManagers);

				NStr::CStr HostName = NProcess::NPlatform::fg_Process_GetHostName();
				TCSet<CStr> ExpectedAppManagers;
				for (auto &Info : AllAppManagers)
					ExpectedAppManagers[("{}/{}:{}"_f << Info.m_HostID << HostName << (Info.m_RootDirectory)).f_GetStr()];

				TCSet<CStr> ActualAppManagers;
				for (auto &AppManager : AppManagers)
					ActualAppManagers[("{}/{}:{}"_f << AppManagers.fs_GetKey(AppManager) << AppManager.m_HostName << AppManager.m_ProgramDirectory).f_GetStr()];

				DMibExpect(ActualAppManagers, ==, ExpectedAppManagers);
			}
			{
				DMibTestPath("Update Independent");
				fSetUpdateType("TestApp", "Independent");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
			}
			{
				DMibTestPath("Update OneAtATime");
				fSetUpdateType("TestApp", "OneAtATime");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1);
			}
			{
				DMibTestPath("Update AllAtOnce");
				fSetUpdateType("TestApp", "AllAtOnce");
				UpdateNotificationState.f_Clear();
				DMibTestMark;
				fUpdateTestApp({"TestTag"});
				DMibTestMark;
				fWaitForAllUpdated("TestApp");

				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
				DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);
			}
			{
				TCActorResultVector<void> AddAppResults;
				for (auto &AppManager : AppManagers)
				{
					CAppManagerInterface::CApplicationAdd Add;
					CAppManagerInterface::CApplicationSettings Settings;
					Settings.m_VersionManagerApplication = "TestApp";
					Settings.m_AutoUpdateTags = fg_CreateSet<CStr>("TestTag2");
					Settings.m_UpdateGroup = "TestGroup2";
					Add.m_Version = PackageInfo.m_VersionID;

					AppManager.f_CallActor(&CAppManagerInterface::f_Add)("TestApp2", Add, Settings) > AddAppResults.f_AddResult();
				}
				fg_CombineResults(AddAppResults.f_GetResults().f_CallSync(g_Timeout));
			}

			if (!NMib::NTest::fg_GroupActive("Expensive"))
				return;

			for (mint i = 0; i < 2; ++i)
			{
				CStr Path;
				if (i == 0)
				{
					Path = "Same Update Group";
					DMibTestMark;
					fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					fSetUpdateGroup("TestApp2", "TestGroup");
				}
				else
				{
					Path = "Separate Update Group";
					DMibTestMark;
					fSetUpdateGroup("TestApp", "TestGroup");
					DMibTestMark;
					fSetUpdateGroup("TestApp2", "TestGroup2");
				}
				DMibTestPath(Path);
				{
					DMibTestPath("AllAtOnce AllAtOnce");
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "AllAtOnce");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					if (i == 0)
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 2u);
					else
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess, ==, nAppManagers * 2u);
					if (i == 0)
						DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, nAppManagers * 2u);
					else
						DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers * 2u);
				}
				{
					DMibTestPath("OneAtATime OneAtATime");
					fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess, ==, nAppManagers * 2u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);
				}
				{
					DMibTestPath("AllAtOnce OneAtATime");
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "OneAtATime");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag", "TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgress, >=, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);

					DMibExpect(UpdateNotificationState.m_AllApplications.m_nSuccess, ==, nAppManagers * 2u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_nMaxInProgress, >=, 1u);
					DMibExpect(UpdateNotificationState.m_AllApplications.m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], >=, 1u);
				}
				{
					DMibTestPath("AllAtOnce Independent");
					fSetUpdateType("TestApp", "AllAtOnce");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStageCoordination[CAppManagerInterface::EUpdateStage_StopOldApp], ==, nAppManagers);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("OneAtATime Independent");
					fSetUpdateType("TestApp", "OneAtATime");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_MaxInStage[CAppManagerInterface::EUpdateStage_StopOldApp], ==, 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
				{
					DMibTestPath("Independent Independent");
					fSetUpdateType("TestApp", "Independent");
					DMibTestMark;
					fSetUpdateType("TestApp2", "Independent");
					UpdateNotificationState.f_Clear();
					DMibTestMark;
					fUpdateTestApp({});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag"});
					DMibTestMark;
					fTagApp("TestApp", {"TestTag2"});
					DMibTestMark;
					fWaitForAllUpdated("TestApp");
					DMibTestMark;
					fWaitForAllUpdated("TestApp2");

					if (i == 0)
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 2u);
					else
						DMibExpect(UpdateNotificationState.m_nMaxAppsInProgressPerAppManager, ==, 1u);

					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp"].m_nMaxInProgress, >= , 1u);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nSuccess, ==, nAppManagers);
					DMibExpect(UpdateNotificationState.m_Applications["TestApp2"].m_nMaxInProgress, >= , 1u);
				}
			}
		};
	}
};

DMibTestRegister(CAppManager_Tests, Malterlib::Cloud);
