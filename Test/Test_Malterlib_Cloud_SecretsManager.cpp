
#include <Mib/Core/Core>
#include <Mib/Concurrency/Actor/Timer>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Cloud/App/KeyManager>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cloud/SecretsManagerUpload>
#include <Mib/Cloud/SecretsManagerDownload>
#include <Mib/Cloud/App/SecretsManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
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
using namespace NMib::NStorage;
using namespace NMib::NNetwork;

#define DTestSecretsManagerEnableLogging 0

static fp64 g_Timeout = 60.0;

namespace
{
	// This class makes it possible to control the order of events for parallel uploads and downloads
	template <typename t_CStreamType = NStream::CBinaryStreamDefault>
	class TCBinaryStreamFileDelayed : public TCBinaryStreamFile<t_CStreamType>
	{
		typedef TCBinaryStreamFile<t_CStreamType> CParent;

	protected:
		DMibStreamImplementProtected(TCBinaryStreamFileDelayed);
	public:
		DMibStreamImplementOperators(TCBinaryStreamFileDelayed);

		CFile m_File;
		NThread::CEvent *m_pOpenEvent;
		NThread::CEvent *m_pCloseEvent;

		TCBinaryStreamFileDelayed(NThread::CEvent *_pOpenEvent, NThread::CEvent *_pCloseEvent)
			: m_pOpenEvent(_pOpenEvent)
			, m_pCloseEvent(_pCloseEvent)
		{
		}

		template <typename tf_CStr>
		void f_Open(const tf_CStr &_FileName, EFileOpen _OpenFlags, EFileAttrib _Attributes = EFileAttrib_None)
		{
			if (m_pOpenEvent && fg_StrFindReverse(_FileName, ".txt") != -1)
			{
				m_pOpenEvent->f_WaitTimeout(60.0);
			}
			CParent::m_File.f_Open(_FileName, _OpenFlags, _Attributes);
		}

		void f_Close()
		{
			if (m_pCloseEvent)
			{
				m_pCloseEvent->f_WaitTimeout(60.0);
			}
			CParent::m_File.f_Close();
		}
	};

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
};

class CSecretsManager_Tests : public NMib::NTest::CTest
{
public:
	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			fp_DoGeneralTests();
		};
	}

	void fp_DoGeneralTests()
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

#if DTestSecretsManagerEnableLogging
		fg_GetSys()->f_AddStdErrLogger();
#endif

		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
		CStr RootDirectory = ProgramDirectory + "/SecretsTests";
		auto SecretsManagerPermissionsForTest = fg_CreateMap<CStr, CPermissionRequirements>("SecretsManager/CommandAll", "SecretsManager/*/*/*");

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
		Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CSecretsManager::mc_pDefaultNamespace);
		Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(g_Timeout);

		TCActor<CDistributedApp_LaunchHelper> LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestSecretsManagerEnableLogging);
		auto Cleanup = g_OnScopeExit > [&]
			{
				LaunchHelper->f_BlockDestroy();
			}
		;

		// Launch KeyManager
		CStr KeyManagerDirectory = RootDirectory + "/KeyManager";
		CFile::fs_CreateDirectory(KeyManagerDirectory);
		CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/KeyManager", KeyManagerDirectory, nullptr);

		// Copy Cloud Client for debugging
		CStr CloudClientDirectory = RootDirectory + "/MalterlibCloud";
		CFile::fs_CreateDirectory(CloudClientDirectory);
		CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/MalterlibCloud", CloudClientDirectory, nullptr);

		// Copy SecretsManagers to their directories
		mint nSecretsManagers = 1;
		{
			TCActorResultVector<void> SecretsManagerLaunchesResults;
			TCVector<TCActor<CSeparateThreadActor>> FileActors;
			for (mint i = 0; i < nSecretsManagers; ++i)
			{
				auto &FileActor = FileActors.f_Insert() = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("File actor"));
				g_Dispatch(FileActor) / [=]
					{
						CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", i);
						CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
						CFile::fs_CreateDirectory(SecretsManagerDirectory);
						//CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/SecretsManager", SecretsManagerDirectory, nullptr);
					}
					> SecretsManagerLaunchesResults.f_AddResult()
				;
			}
			fg_CombineResults(SecretsManagerLaunchesResults.f_GetResults().f_CallSync());
		}

		// Launch SecretsManagers
		TCActorResultVector<CDistributedApp_LaunchInfo> SecretsManagerLaunchesResults;
		TCVector<CDistributedApp_LaunchInfo> SecretsManagerLaunches;
		
		auto fLaunchSecretManagers = [&]
			{
				SecretsManagerLaunches.f_Clear();
				SecretsManagerLaunchesResults = {};

				for (mint i = 0; i < nSecretsManagers; ++i)
				{
					CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", i);
					CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
					LaunchHelper
						(
							&CDistributedApp_LaunchHelper::f_LaunchInProcess
							, SecretsManagerName
							, SecretsManagerDirectory
							, &fg_ConstructApp_SecretsManager
							, NContainer::TCVector<NStr::CStr>{}
						)
						> SecretsManagerLaunchesResults.f_AddResult()
					;
				}
				for (auto &LaunchResult : SecretsManagerLaunchesResults.f_GetResults().f_CallSync(g_Timeout))
					SecretsManagerLaunches.f_Insert(fg_Move(*LaunchResult));
			}
		;
		fLaunchSecretManagers();
		
		auto KeyManagerLaunch = LaunchHelper
			(
				&CDistributedApp_LaunchHelper::f_LaunchInProcess
				, "KeyManager"
				, KeyManagerDirectory
				, &fg_ConstructApp_KeyManager
				, NContainer::TCVector<NStr::CStr>{}
			)
			.f_CallSync(g_Timeout)
		;
		DMibExpect(KeyManagerLaunch.m_HostID, !=, "");

		// Setup KeyManager
		auto pKeyManagerTrust = KeyManagerLaunch.m_pTrustInterface;
		auto &KeyManagerTrust = *pKeyManagerTrust;
		CStr KeyManagerHostID = KeyManagerLaunch.m_HostID;

		// Add listen socket that secret managers can connect to
		CDistributedActorTrustManager_Address KeyManagerServerAddress;
		KeyManagerServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/Keymanager.sock"_f << KeyManagerDirectory);
		DMibCallActor(KeyManagerTrust, CDistributedActorTrustManagerInterface::f_AddListen, KeyManagerServerAddress).f_CallSync(g_Timeout);

		auto HelperActor = fg_ConcurrentActor();
		CCurrentActorScope CurrentActor{HelperActor};

		{
			TCActor<CProcessLaunchActor> KeyManagerCommandLine = fg_Construct();
			CProcessLaunchActor::CSimpleLaunch LaunchParams{KeyManagerDirectory + "/KeyManager", {"--provide-password"}};
			LaunchParams.m_DestructFlags = EProcessLaunchCloseFlag_BlockOnExit;
			LaunchParams.m_ToLog = CProcessLaunchActor::ELogFlag_All;
#if DTestSecretsManagerEnableLogging
			LaunchParams.m_ToLog |= CProcessLaunchActor::ELogFlag_AdditionallyOutputToStdErr;
#endif
			TCPromise<void> LaunchedPromise;
			TCPromise<void> ExitedPromise;

			LaunchParams.m_Params.m_fOnStateChange = [LaunchedPromise, ExitedPromise](CProcessLaunchStateChangeVariant const &_State, fp64 _TimeSinceStart)
				{
					switch (_State.f_GetTypeID())
					{
					case NProcess::EProcessLaunchState_Launched:
						{
							LaunchedPromise.f_SetResult();
						}
						break;
					case NProcess::EProcessLaunchState_LaunchFailed:
						{
							auto &LaunchError = _State.f_Get<NProcess::EProcessLaunchState_LaunchFailed>();
							LaunchedPromise.f_SetException(DMibErrorInstance(LaunchError));
						}
						break;
					case NProcess::EProcessLaunchState_Exited:
						{
							auto ExitStatus = _State.f_Get<NProcess::EProcessLaunchState_Exited>();
							if (ExitStatus)
								ExitedPromise.f_SetException(DMibErrorInstance(fg_Format("Launch failed: Status {}", ExitStatus)));
							else
								ExitedPromise.f_SetResult();
						}
						break;
					}
				}
			;
			auto LaunchSubscription = KeyManagerCommandLine(&CProcessLaunchActor::f_Launch, fg_Move(LaunchParams), HelperActor).f_CallSync(g_Timeout);
			LaunchedPromise.f_Dispatch().f_CallSync(g_Timeout);
			KeyManagerCommandLine(&CProcessLaunchActor::f_SendStdIn, "Password\n").f_CallSync(g_Timeout);
			ExitedPromise.f_Dispatch().f_CallSync(g_Timeout);
			KeyManagerCommandLine->f_Destroy().f_CallSync(g_Timeout);
		}

		// Setup trust for SecretsManagers
		
		struct CSecretsManagerInfo
		{
			CStr const &f_GetHostID() const
			{
				return TCMap<CStr, CSecretsManagerInfo>::fs_GetKey(*this);
			}
			
			TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
			CDistributedActorTrustManager_Address m_Address;
		};

		TCSet<CStr> AllSecretsManagerHosts;
		TCMap<CStr, CSecretsManagerInfo> AllSecretsManagers;
		auto fSetupListen = [&]
			{
				AllSecretsManagerHosts.f_Clear();
				AllSecretsManagers.f_Clear();
				TCActorResultVector<void> ListenResults;
				mint iSecretsManager = 0;
				for (auto &SecretsManager : SecretsManagerLaunches)
				{
					CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", iSecretsManager);
					CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
					
					AllSecretsManagerHosts[SecretsManager.m_HostID];
					auto &SecretsManagerInfo = AllSecretsManagers[SecretsManager.m_HostID];
					SecretsManagerInfo.m_pTrustInterface = SecretsManager.m_pTrustInterface;

					SecretsManagerInfo.m_Address.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/SecretsManagerTest.sock"_f << SecretsManagerDirectory);
					DMibCallActor(*SecretsManager.m_pTrustInterface, CDistributedActorTrustManagerInterface::f_AddListen, SecretsManagerInfo.m_Address) > ListenResults.f_AddResult();
					++iSecretsManager;
				}
				fg_CombineResults(ListenResults.f_GetResults().f_CallSync(g_Timeout));
			}
		;
		fSetupListen();
		
		TCActorResultVector<void> SetupTrustResults;

		static auto constexpr c_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;
		auto fPermissionsAdd = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, c_WaitForSubscriptions};
			}
		;
		auto fPermissionsRemove = [](auto &&_HostID, auto &&_Permissions)
			{
				return CDistributedActorTrustManagerInterface::CRemovePermissions{{_HostID, ""}, _Permissions, c_WaitForSubscriptions};
			}
		;
		auto fNamespaceHosts = [](auto &&_Namespace, auto &&_Hosts)
			{
				return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, c_WaitForSubscriptions};
			}
		;

		for (auto &SecretsManager : AllSecretsManagers)
		{
			auto pSecretsManagerTrust = SecretsManager.m_pTrustInterface;
			auto &SecretsManagerTrust = *pSecretsManagerTrust;
			CStr SecretsManagerHostID = SecretsManager.f_GetHostID();
			auto TrustSecretsManagers = AllSecretsManagerHosts;
			TrustSecretsManagers.f_Remove(SecretsManagerHostID);
#if 0
			DMibCallActor
				(
					SecretsManagerTrust
					, CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace
					, fNamespaceHosts("com.malterlib/Cloud/SecretsManagerCoordination", TrustSecretsManagers)
				)
				> SetupTrustResults.f_AddResult()
			;
#endif
			DMibCallActor
				(
					SecretsManagerTrust
					, CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace
				 	, fNamespaceHosts(CKeyManager::mc_pDefaultNamespace, fg_CreateSet<CStr>(KeyManagerHostID))
				)
				> SetupTrustResults.f_AddResult()
			;

			DMibCallActor
				(
					TrustManager
					, CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CSecretsManager::mc_pDefaultNamespace
					, fg_CreateSet<CStr>(SecretsManagerHostID)
				 	, c_WaitForSubscriptions
				)
				> SetupTrustResults.f_AddResult()
			;

			DMibCallActor
				(
					SecretsManagerTrust
					, CDistributedActorTrustManagerInterface::f_AddPermissions
					, fPermissionsAdd(TestHostID, SecretsManagerPermissionsForTest)
				)
				> SetupTrustResults.f_AddResult()
			;

			for (auto &SecretsManagerInner : AllSecretsManagers)
			{
				CStr SecretsManagerHostIDInner = SecretsManagerInner.f_GetHostID();
				if (SecretsManagerHostIDInner == SecretsManagerHostID)
					continue;
				
				auto pSecretsManagerTrustInner = SecretsManagerInner.m_pTrustInterface;
				
				TCPromise<void> Promise;
				DMibCallActor
					(
					 	SecretsManagerTrust
					 	, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket
					 	, CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{SecretsManager.m_Address}
					)
					> Promise / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
					{
						auto &SecretsManagerTrustInner = *pSecretsManagerTrustInner;
						DMibCallActor(SecretsManagerTrustInner, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
					}
				;
				Promise.f_Dispatch() > SetupTrustResults.f_AddResult();

			}

			TCPromise<void> Promise;
			DMibCallActor
				(
				 	KeyManagerTrust
				 	, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket
				 	, CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{KeyManagerServerAddress}
				)
				> Promise / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
				{
					auto &SecretsManagerTrust = *pSecretsManagerTrust;
					DMibCallActor(SecretsManagerTrust, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1) > Promise.f_ReceiveAny();
				}
			;
			Promise.f_Dispatch() > SetupTrustResults.f_AddResult();
		}

		SetupTrustResults.f_GetResults().f_CallSync(g_Timeout);

		CSecretsManager::CSecret StringSecret{"Secret1"};
		CSecretsManager::CSecret ByteVectorSecret{CSecureByteVector{(uint8 const *)"Secret2", 7}};
		CSecretsManager::CSecret FileSecret{CSecretsManager::CSecretFile{}};

		{
			DMibTestPath("General");
			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			auto SecretsManagers = Subscriptions.f_SubscribeMultiple<CSecretsManager>(nSecretsManagers);
			auto SecretsManager = SecretsManagers[0];

			{
				DMibTestPath("Test getters and setter");
				
				CSecretsManager::CSecretProperties Properties;

				// Unset values - no exceptions from TCOptional
				DMibExpect(Properties.f_GetSecret(), ==, CSecretsManager::CSecret{});
				DMibExpect(Properties.f_GetUserName(), ==, NStr::CStrSecure{});
				DMibExpect(Properties.f_GetURL(), ==, NStr::CStrSecure{});
				DMibExpect(Properties.f_GetExpires(), ==, NTime::CTime{});
				DMibExpect(Properties.f_GetNotes(), ==, NStr::CStrSecure{});
				DMibExpect(Properties.f_GetMetadata(), ==, (TCMap<NStr::CStrSecure, CEJSON>{}));
				DMibExpect(Properties.f_GetCreated(), ==, NTime::CTime{});
				DMibExpect(Properties.f_GetModified(), ==, NTime::CTime{});
				DMibExpect(Properties.f_GetSemanticID(), ==, NStr::CStrSecure{});
				DMibExpect(Properties.f_GetTags(), ==, TCSet<NStr::CStrSecure>{});

				CSecretsManager::CSecretProperties Properties2
					{
						CSecretsManager::CSecretProperties{}
						.f_SetSecret(CSecretsManager::CSecret{"text"})
						.f_SetUserName("Username")
						.f_SetURL("URL")
						.f_SetExpires(NTime::CTimeConvert::fs_CreateTime(1974, 4, 4))
						.f_SetNotes("Note")
						.f_SetMetadata("a", "b")
						.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1975, 5, 5))
						.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1976, 6, 6))
						.f_SetSemanticID("SemanticID")
						.f_AddTags({"Tag1", "Tag2"})
					}
				;
				DMibExpect(Properties2.f_GetSecret(), ==, CSecretsManager::CSecret{"text"});
				DMibExpect(Properties2.f_GetUserName(), ==, "Username");
				DMibExpect(Properties2.f_GetURL(), ==, "URL");
				DMibExpect(Properties2.f_GetExpires(), ==, NTime::CTimeConvert::fs_CreateTime(1974, 4, 4));
				DMibExpect(Properties2.f_GetNotes(), ==, "Note");
				DMibExpect(Properties2.f_GetMetadata(), ==, (TCMap<NStr::CStrSecure, CEJSON>{{"a", "b"}}));
				DMibExpect(Properties2.f_GetCreated(), ==, NTime::CTimeConvert::fs_CreateTime(1975, 5, 5));
				DMibExpect(Properties2.f_GetModified(), ==, NTime::CTimeConvert::fs_CreateTime(1976, 6, 6));
				DMibExpect(Properties2.f_GetSemanticID(), ==, "SemanticID");
				DMibExpect(Properties2.f_GetTags(), ==, (TCSet<NStr::CStrSecure>{{"Tag1", "Tag2"}}));
			}

			//
			// Set up a number of secrets and send them to the manager
			//
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder1", "Name1"}
					, CSecretsManager::CSecretProperties{}
					.f_SetSecret(fg_TempCopy(StringSecret))
					.f_SetUserName("UserName")
					.f_SetURL("http://URL/")
					.f_SetExpires(NTime::CTimeConvert::fs_CreateTime(1971, 1, 1))
					.f_SetNotes("Testing11")
					.f_SetMetadata("Key", "Value")
					.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1972, 2, 2))
					.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1973, 3, 3))
					.f_SetSemanticID("Semantic1")
					.f_SetTags({"Shared1", "Unique1"})
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder1", "Name2"}
					, CSecretsManager::CSecretProperties{}
					.f_SetNotes("Testing12")
					.f_SetSecret(fg_TempCopy(ByteVectorSecret))
					.f_SetTags({"Shared1", "Shared2", "Unique2"})
					.f_SetSemanticID("Semantic2")
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder2", "Name1"}
					, CSecretsManager::CSecretProperties{}.f_SetNotes("Testing21").f_SetSecret(fg_TempCopy(FileSecret)).f_SetTags({"Shared2"}).f_SetSemanticID("Semantic2")
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder2", "Name2"}
					, CSecretsManager::CSecretProperties{}.f_SetNotes("Testing22").f_SetMetadata("Key1", "Value1").f_SetTags({"Unique3"}).f_SetSemanticID("Semantic4")
				)
				.f_CallSync(g_Timeout)
			;

			auto fGetSecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name) -> CSecretsManager::CSecret
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecret, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;
			auto fGetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name) -> CSecretsManager::CSecretProperties
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;
			auto fSetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, CSecretsManager::CSecretProperties &&_Properties)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_SetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}, fg_Move(_Properties) ).f_CallSync(g_Timeout);
				}
			;
			auto fGetBySemantic = [&](NStr::CStrSecure &&_SemanticID, TCSet<NStr::CStr> const &_Tags) -> CSecretsManager::CSecret
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecretBySemanticID, _SemanticID, _Tags).f_CallSync(g_Timeout);
				}
			;
			auto fEnumerateFor = [&](TCOptional<NStr::CStrSecure> _ID, TCSet<NStr::CStr> const &_Tags) -> TCSet<CSecretsManager::CSecretID>
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_EnumerateSecrets, _ID, _Tags).f_CallSync(g_Timeout);
				}
			;
			auto fAddTagsAndGetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, TCSet<NStr::CStr> const &_RemoveTags, TCSet<NStr::CStr> const &_AddTags)
				-> CSecretsManager::CSecretProperties
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_ModifyTags, CSecretsManager::CSecretID{_Folder, _Name}, _RemoveTags, _AddTags).f_CallSync(g_Timeout);
					return fGetProperties(_Folder, _Name);
				}
			;
			auto fSetKeyValue = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key, CEJSON const &_Value)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_SetMetadata, CSecretsManager::CSecretID{_Folder, _Name}, fg_TempCopy(_Key), fg_TempCopy(_Value)).f_CallSync(g_Timeout);
				}
			;
			auto fRemoveKey = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_RemoveMetadata, CSecretsManager::CSecretID{_Folder, _Name}, fg_TempCopy(_Key)).f_CallSync(g_Timeout);
				}
			;
			auto fRemoveSecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_RemoveSecret, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;

			{
				//
				// Verify that we get the correct secrets from different folders and Names
				//
				DMibTestPath("Get Properties");
				
				// Verify all properties in the first secret
				auto Properties = fGetProperties("Folder1", "Name1");
				DMibExpect(*Properties.m_Secret, ==, StringSecret);
				DMibExpect(*Properties.m_UserName, ==, "UserName");
				DMibExpect(*Properties.m_URL, ==, "http://URL/");
				DMibExpect(*Properties.m_Expires, ==, NTime::CTimeConvert::fs_CreateTime(1971, 1, 1));
				DMibExpect(*Properties.m_Notes, ==, "Testing11");
				DMibExpect(*Properties.m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key", "Value"}}));
				DMibExpect(*Properties.m_Created, ==, NTime::CTimeConvert::fs_CreateTime(1972, 2, 2));
				DMibExpect(*Properties.m_Modified, ==, NTime::CTimeConvert::fs_CreateTime(1973, 3, 3));
				DMibExpect(*Properties.m_SemanticID, ==, "Semantic1");
				DMibExpect(*Properties.m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				
				DMibExpect(*fGetProperties("Folder1", "Name2").m_Notes, ==, "Testing12");
				DMibExpect(*fGetProperties("Folder2", "Name1").m_Notes, ==, "Testing21");
				DMibExpect(*fGetProperties("Folder2", "Name2").m_Notes, ==, "Testing22");

				DMibExpectException(fGetProperties("Folder1", "NoMatch"), DMibErrorInstance("No secret matching ID: 'Folder1/NoMatch'"));
				DMibExpectException(fGetProperties("NoMatch", "Name1"), DMibErrorInstance("No secret matching ID: 'NoMatch/Name1'"));
			}
			
			{
				DMibTestPath("Get Secrets");

				DMibExpect(fGetSecret("Folder1", "Name1"), ==, StringSecret);
				DMibExpect(fGetSecret("Folder1", "Name2"), ==, ByteVectorSecret);
				DMibExpect(fGetSecret("Folder2", "Name1"), ==, FileSecret);
				// No secret set - we should get a NotSet secret
				DMibExpect(fGetSecret("Folder2", "Name2"), ==, CSecretsManager::CSecret{});
				
				DMibExpectException(fGetSecret("NoMatch", "Name2"), DMibErrorInstance("No secret matching ID: 'NoMatch/Name2'"));
				DMibExpectException(fGetSecret("Folder1", "NoMatch"), DMibErrorInstance("No secret matching ID: 'Folder1/NoMatch'"));

			}
			
			{
				DMibTestPath("Test ModifyTags");

				// Verify that we have the tags that we inserted in the secret properties
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				DMibExpect(*fGetProperties("Folder1", "Name2").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Shared2", "Unique2"}}));
				DMibExpect(*fGetProperties("Folder2", "Name1").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared2"}}));
				
				// No op
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {},            {}                      )).m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				// Add tags
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {},            {{"Added"}}             )).m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1", "Added"}}));
				// Remove
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Shared1"}}, {}                      )).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Added"}}));
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Unique1"}}, {}                      )).m_Tags, ==, (TCSet<NStr::CStr>{{"Added"}}));
				// Remove and Add
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Added"}},   {{"Shared1", "Unique1"}})).m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				// Remove non-present tag
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Added"}},   {}                      )).m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
	
				// Add to Secret with no existing tags
				DMibExpect(*(fAddTagsAndGetProperties("Folder2", "Name2", {},            {{"Unique3"}}           )).m_Tags, ==, TCSet<NStr::CStr>{{"Unique3"}});

				// Check for exception for both missing folder and name
				auto fModifySecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, TCSet<NStr::CStr> const &_Remove, TCSet<NStr::CStr> const &_Add)
					{
						DMibCallActor(SecretsManager, CSecretsManager::f_ModifyTags, CSecretsManager::CSecretID{_Folder, _Name}, _Remove, _Add).f_CallSync(g_Timeout);
					}
				;
				DMibExpectException(fModifySecret("NoMatch", "Name1", {}, {}), DMibErrorInstance("No secret matching ID: 'NoMatch/Name1'"));
				DMibExpectException(fModifySecret("Folder1", "NoMatch", {}, {}), DMibErrorInstance("No secret matching ID: 'Folder1/NoMatch'"));
				
				DMibExpectException(fModifySecret("Folder1", "Name1", {{"Not/Allowed"}}, {}),                DMibErrorInstance("Malformed Tag: 'Not/Allowed'"));
				DMibExpectException(fModifySecret("Folder1", "Name1", {},                {{"Not/Allowed"}}), DMibErrorInstance("Malformed Tag: 'Not/Allowed'"));
			}
			
			{
				DMibTestPath("GetSecrets by SemanticID");

				DMibExpect(fGetBySemantic("Semantic1", {}),                       ==, StringSecret);
				DMibExpect(fGetBySemantic("Semantic1", {{"Unique1"}}),            ==, StringSecret);
				DMibExpect(fGetBySemantic("Semantic1", {{"Shared1"}}),            ==, StringSecret);
				DMibExpect(fGetBySemantic("Semantic1", {{"Unique1", "Shared1"}}), ==, StringSecret);
				DMibExpect(fGetBySemantic("Semantic2", {{"Unique2"}}),            ==, ByteVectorSecret);
				// Semantic4 has no secret set - check for a NotSet secret
				DMibExpect(fGetBySemantic("Semantic4", {}),                       ==, CSecretsManager::CSecret{});

				DMibExpectException(fGetBySemantic("NoMatch", {}), DMibErrorInstance("No secret matching Semantic ID: 'NoMatch'"));
				DMibExpectException
					(
						fGetBySemantic("Semantic1", {{"Unique1", "Shared1", "NoMatch"}})
						, DMibErrorInstance("No secret matching Semantic ID: 'Semantic1' and Tags: 'NoMatch', 'Shared1', 'Unique1'")
					)
				;
				
				DMibExpectException(fGetBySemantic("Semantic2", {}),          DMibErrorInstance("Multiple secrets matching Semantic ID: 'Semantic2'"));
				DMibExpectException(fGetBySemantic("Semantic2", {"Shared2"}), DMibErrorInstance("Multiple secrets matching Semantic ID: 'Semantic2' and Tag: 'Shared2'"));
			}
			
			{
				DMibTestPath("Test Enumeration");

				DMibExpect(fEnumerateFor({},              {}),            ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}, {"Folder1", "Name2"}, {"Folder2", "Name1"}, {"Folder2", "Name2"}}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {}),            ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {{"Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {{"NoMatch"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));
				DMibExpect(fEnumerateFor({},              {{"Shared1"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}, {"Folder1", "Name2"}}));
				DMibExpect(fEnumerateFor({},              {{"Shared2"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder2", "Name1"}, {"Folder1", "Name2"}}));
				DMibExpect(fEnumerateFor({},              {{"NoMatch"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));
				DMibExpect(fEnumerateFor({{"NoMatch"}},   {}),            ==, (TCSet<CSecretsManager::CSecretID>{}));
			}
			
			{
				DMibTestPath("Test metadata");
				

				auto fSetKeyValueAndGet = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key, CEJSON const &_Value) -> CSecretsManager::CSecretProperties
					{
						fSetKeyValue(_Folder, _Name, _Key, _Value);
						return fGetProperties(_Folder, _Name);
					}
				;
				
				auto fRemoveKeyAndGet = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key) -> CSecretsManager::CSecretProperties
					{
						fRemoveKey(_Folder, _Name, _Key);
						return fGetProperties(_Folder, _Name);
					}
				;
				
				// Verify original value
				DMibExpect(*fGetProperties("Folder2", "Name2").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key1", "Value1"}}));

				DMibExpect(*fSetKeyValueAndGet("Folder2", "Name2", "Key2", "Value2").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key1", "Value1"}, {"Key2", "Value2"}}));
				DMibExpect(*fSetKeyValueAndGet("Folder2", "Name1", "Key3", "Value3").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key3", "Value3"}}));
				DMibExpectException(fSetKeyValue("Folder1", "NoMatch", "Key3", "Value3"), DMibErrorInstance("No secret matching ID: 'Folder1/NoMatch'"));

				DMibExpect(*fRemoveKeyAndGet("Folder2", "Name2", "Key1").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key2", "Value2"}}));
				DMibExpectException(fRemoveKey("Folder1", "NoMatch", "Key3"), DMibErrorInstance("No secret matching ID: 'Folder1/NoMatch'"));
			}
			
			{
				DMibTestPath("Test SetProperties");

				{
					auto TimeBeforeSetPropertiesCall = NTime::CTime::fs_NowUTC();
					NMib::NSys::fg_Thread_Sleep(NMib::NTime::NPlatform::fg_TimeRaw_Resolution());
					DMibCallActor
						(
							SecretsManager
							, CSecretsManager::f_SetSecretProperties
							, CSecretsManager::CSecretID{"Test", "Test1"}
							, CSecretsManager::CSecretProperties{}
						 )
						.f_CallSync(g_Timeout)
					;
					NMib::NSys::fg_Thread_Sleep(NMib::NTime::NPlatform::fg_TimeRaw_Resolution());
					auto TimeAfterSetPropertiesCall = NTime::CTime::fs_NowUTC();

					// If Created or Modified were not set, ensure they are updated correctly
					auto Properties = fGetProperties("Test", "Test1");
					DMibExpect(*Properties.m_Created, >, TimeBeforeSetPropertiesCall);
					DMibExpect(*Properties.m_Created, <, TimeAfterSetPropertiesCall);
					DMibExpect(*Properties.m_Created, ==, *Properties.m_Modified);
				}

				{
					{
						auto TimeBeforeSetPropertiesCall = NTime::CTime::fs_NowUTC();
						NMib::NSys::fg_Thread_Sleep(NMib::NTime::NPlatform::fg_TimeRaw_Resolution());
						DMibCallActor
							(
								SecretsManager
								, CSecretsManager::f_SetSecretProperties
								, CSecretsManager::CSecretID{"Test", "Test1"}
								, CSecretsManager::CSecretProperties{}
								.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1972, 2, 2))
							 )
							.f_CallSync(g_Timeout)
						;
						NMib::NSys::fg_Thread_Sleep(NMib::NTime::NPlatform::fg_TimeRaw_Resolution());
						auto TimeAfterSetPropertiesCall = NTime::CTime::fs_NowUTC();

						// Verify that Created was set correctly and Modified updated
						auto Properties = fGetProperties("Test", "Test1");
						DMibExpect(*Properties.m_Created, ==, NTime::CTimeConvert::fs_CreateTime(1972, 2, 2));
						DMibExpect(*Properties.m_Modified, >, TimeBeforeSetPropertiesCall);
						DMibExpect(*Properties.m_Modified, <, TimeAfterSetPropertiesCall);
					}
					{
						DMibCallActor
							(
								SecretsManager
								, CSecretsManager::f_SetSecretProperties
								, CSecretsManager::CSecretID{"Test", "Test1"}
								, CSecretsManager::CSecretProperties{}
								.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1973, 3, 3))
							 )
							.f_CallSync(g_Timeout)
						;
						// Verify that Modified was set correctly and that Created retained its value
						auto Properties2 = fGetProperties("Test", "Test1");
						DMibExpect(*Properties2.m_Created, ==, NTime::CTimeConvert::fs_CreateTime(1972, 2, 2));
						DMibExpect(*Properties2.m_Modified, ==, NTime::CTimeConvert::fs_CreateTime(1973, 3, 3));
					}
				}
				
				{
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("Not/Allowed")), DMibErrorInstance("Malformed Semantic ID: 'Not/Allowed'"));
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetTags({"Not/Allowed"})), DMibErrorInstance("Malformed Tag: 'Not/Allowed'"));

					{
						auto fSetPropertiesTestUntouched = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, CSecretsManager::CSecretProperties &&_PropertiesToSet)
							{
								auto ContentBeforeSetProperties = fGetProperties(_Folder, _Name);
								fSetProperties(_Folder, _Name, fg_TempCopy(_PropertiesToSet));
								auto ContentAfterSetProperties = fGetProperties(_Folder, _Name);

								// If a property was set in _PropertiesToSet test against that value otherwise against the ContentBeforeSetProperties value
								if (_PropertiesToSet.m_Secret)
									DMibExpect(ContentAfterSetProperties.f_GetSecret(), ==, _PropertiesToSet.f_GetSecret());
								else
									DMibExpect(ContentAfterSetProperties.f_GetSecret(), ==, ContentBeforeSetProperties.f_GetSecret());

								if (_PropertiesToSet.m_UserName)
									DMibExpect(ContentAfterSetProperties.f_GetUserName(), ==, _PropertiesToSet.f_GetUserName());
								else
									DMibExpect(ContentAfterSetProperties.f_GetUserName(), ==, ContentBeforeSetProperties.f_GetUserName());

								if (_PropertiesToSet.m_URL)
									DMibExpect(ContentAfterSetProperties.f_GetURL(), ==, _PropertiesToSet.f_GetURL());
								else
									DMibExpect(ContentAfterSetProperties.f_GetURL(), ==, ContentBeforeSetProperties.f_GetURL());

								if (_PropertiesToSet.m_Expires)
									DMibExpect(ContentAfterSetProperties.f_GetExpires(), ==, _PropertiesToSet.f_GetExpires());
								else
									DMibExpect(ContentAfterSetProperties.f_GetExpires(), ==, ContentBeforeSetProperties.f_GetExpires());

								if (_PropertiesToSet.m_Notes)
									DMibExpect(ContentAfterSetProperties.f_GetNotes(), ==, _PropertiesToSet.f_GetNotes());
								else
									DMibExpect(ContentAfterSetProperties.f_GetNotes(), ==, ContentBeforeSetProperties.f_GetNotes());

								if (_PropertiesToSet.m_Metadata)
									DMibExpect(ContentAfterSetProperties.f_GetMetadata(), ==, _PropertiesToSet.f_GetMetadata());
								else
									DMibExpect(ContentAfterSetProperties.f_GetMetadata(), ==, ContentBeforeSetProperties.f_GetMetadata());

								if (_PropertiesToSet.m_Created)
									DMibExpect(ContentAfterSetProperties.f_GetCreated(), ==, _PropertiesToSet.f_GetCreated());
								else
									DMibExpect(ContentAfterSetProperties.f_GetCreated(), ==, ContentBeforeSetProperties.f_GetCreated());

								if (_PropertiesToSet.m_Modified)
									DMibExpect(ContentAfterSetProperties.f_GetModified(), ==, _PropertiesToSet.f_GetModified());
								// No else here, modified is set when the secret is mofied.
								
								if (_PropertiesToSet.m_SemanticID)
									DMibExpect(ContentAfterSetProperties.f_GetSemanticID(), ==, _PropertiesToSet.f_GetSemanticID());
								else
									DMibExpect(ContentAfterSetProperties.f_GetSemanticID(), ==, ContentBeforeSetProperties.f_GetSemanticID());

								if (_PropertiesToSet.m_Tags)
									DMibExpect(ContentAfterSetProperties.f_GetTags(), ==, _PropertiesToSet.f_GetTags());
								else
									DMibExpect(ContentAfterSetProperties.f_GetTags(), ==, ContentBeforeSetProperties.f_GetTags());

							}
						;

						fSetProperties
							(
								"Test"
								, "Test2"
								, CSecretsManager::CSecretProperties{}
								.f_SetSecret(CSecretsManager::CSecret{"text"})
								.f_SetUserName("Username")
								.f_SetURL("URL")
								.f_SetExpires(NTime::CTimeConvert::fs_CreateTime(1974, 4, 4))
								.f_SetNotes("Note")
								.f_SetMetadata("a", "b")
								.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1975, 5, 5))
								.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1976, 6, 6))
								.f_SetSemanticID("SemanticID")
								.f_AddTags({"Tag1", "Tag2"})
							 )
						;
						{
							DMibTestPath("Test SetProperties - only setting Secret");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetSecret(CSecretsManager::CSecret{"newtext"}));
						}
						{
							DMibTestPath("Test SetProperties - only setting Username");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetUserName("NewUsername"));
						}
						{
							DMibTestPath("Test SetProperties - only setting URL");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetURL("NewURL"));
						}
						{
							DMibTestPath("Test SetProperties - only setting Expires");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetExpires(NTime::CTimeConvert::fs_CreateTime(1977, 7, 7)));
						}
						{
							DMibTestPath("Test SetProperties - only setting Created");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1988, 8, 8)));
						}
						{
							DMibTestPath("Test SetProperties - only setting Modified");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1999, 9, 9)));
						}
						{
							DMibTestPath("Test SetProperties - only setting Notes");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetNotes("NewNote"));
						}
						{
							DMibTestPath("Test SetProperties - only setting Metadata");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetMetadata("c", "d"));
						}
						{
							DMibTestPath("Test SetProperties - only setting SemanticID");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetSemanticID("NewSemanticID"));
						}
						{
							DMibTestPath("Test SetProperties - only setting Tags");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetTags({"NewTag1", "NewTag2"}));
						}
					}
				}
				{
					fSetProperties("Removable", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note"));

					DMibExpect(fGetSecret("Removable", "Name1"), ==, CSecretsManager::CSecret{});
					fRemoveSecret("Removable", "Name1");
					DMibExpectException(fGetSecret("Removable", "Name1"), DMibErrorInstance("No secret matching ID: 'Removable/Name1'"));
				}
			}

			
			auto fAddPermissions = [&](TCMap<CStr, CPermissionRequirements> const &_Permissions)
				{
					for (auto &SecretsManager : AllSecretsManagers)
					{
						auto pSecretsManagerTrust = SecretsManager.m_pTrustInterface;
						auto &SecretsManagerTrust = *pSecretsManagerTrust;
						
						DMibCallActor
							(
								SecretsManagerTrust
								, CDistributedActorTrustManagerInterface::f_AddPermissions
								, fPermissionsAdd(TestHostID, _Permissions)
							)
							.f_CallSync(g_Timeout)
						;
					}
				}
			;
			
			auto fAddPermission = [&](CStr const &_Permission)
				{
					fAddPermissions(fg_CreateMap<CStr, CPermissionRequirements>(fg_TempCopy(_Permission)));
				}
			;

			auto fRemovePermissions = [&](TCSet<NStr::CStr> const &_Permissions)
				{
					TCMap<CStr, CPermissionRequirements> Permissions;
					for (auto const &Permission : _Permissions)
						Permissions[Permission];

					for (auto &SecretsManager : AllSecretsManagers)
					{
						auto pSecretsManagerTrust = SecretsManager.m_pTrustInterface;
						auto &SecretsManagerTrust = *pSecretsManagerTrust;

						DMibCallActor
							(
								SecretsManagerTrust
								, CDistributedActorTrustManagerInterface::f_RemovePermissions
								, fPermissionsRemove(TestHostID, Permissions)
							)
							.f_CallSync(g_Timeout)
						;
					}
				}
			;

			auto fRemovePermissionsMap = [&](TCMap<CStr, CPermissionRequirements> const &_Permissions)
				{
					TCSet<NStr::CStr> Permissions;
					for (auto const &Methods : _Permissions)
						Permissions[_Permissions.fs_GetKey(Methods)];
					fRemovePermissions(Permissions);
				}
			;


			// So far we have used the Wildcard and All permissions. Remove them so we can test more fine grained permissions.
			fRemovePermissions(SecretsManagerPermissionsForTest);
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test GetProperties Command Permissions");
				DMibExpectException(fGetProperties("Folder1", "Name1"), DMibErrorInstance("Access denied"));
				
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/GetSecretProperties"
					)
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Secret, ==, StringSecret);

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetProperties Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fGetProperties("Folder1", "Name1"), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}

				// Check that access is permitted again
				DMibExpect(*fGetProperties("Folder1", "Name1").m_UserName, ==, "UserName");

				fRemovePermissions(Needed);
			}
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test GetSecret Command Permissions");
				DMibExpectException(fGetSecret("Folder1", "Name1"), DMibErrorInstance("Access denied"));
				
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/GetSecret"
					)
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(fGetSecret("Folder1", "Name1"), ==, StringSecret);

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetSecret Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fGetSecret("Folder1", "Name1"), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}

				{
					DMibTestPath("Test GetSecret Command Permissions after permissions have been restored");
					// Check that access is permitted again
					DMibExpect(fGetSecret("Folder1", "Name1"), ==, StringSecret);
				}
				fRemovePermissions(Needed);
			}
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test GetSecretBySemanticID Command Permissions");
				DMibExpectException(fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}}), DMibErrorInstance("Access denied"));
				
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/GetSecretBySemanticID"
					)
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}}), ==, StringSecret);
				
				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetSecretBySemanticID Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));
					
					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					// GetBySemanticID and EnumerateSecrets simply skips the secrets one hasn't permission to see so here
					// we can either get an access denied exception or a no secret exception
					if (Needed.fs_GetKey(Permission) == "SecretsManager/Command/GetSecretBySemanticID")
						DMibExpectException(fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}}), DMibErrorInstance("Access denied"));
					else
					{
						DMibExpectException
							(
								 fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}})
								 , DMibErrorInstance("No secret matching Semantic ID: 'Semantic1' and Tags: 'Shared1', 'Unique1'")
							 )
						;
					}
					fAddPermission(Needed.fs_GetKey(Permission));
				}
				
				{
					DMibTestPath("Test GetSecretBySemanticID Command Permissions details");
					// Check that access is permitted again
					DMibExpect(fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}}), ==, StringSecret);
					// Check that we find it for a subset of the tags
					DMibExpect(fGetBySemantic("Semantic1", {{"Unique1"}}           ), ==, StringSecret);
					DMibExpect(fGetBySemantic("Semantic1", {{"Shared1"}}           ), ==, StringSecret);

					// This one is a bit strange, but the semantic ID does not work as the tags, where a subset of tags can find a matching secret.
					// The empty semantic ID will only match secrets with an empty semantic ID
					DMibExpectException(fGetBySemantic("", {{"Shared1", "Unique1"}}), DMibErrorInstance("No secret matching Semantic ID: '' and Tags: 'Shared1', 'Unique1'"));

					// GetSecretBySemanticID and EnumerateSecrets will only find/enumerate secrets where we have permissions for both SemanticID and all Tags on the secret.
					// Missing a permission should result in a no secret, regardless of whichtags we use to search for
					fRemovePermissions({"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"});
					DMibExpectException(fGetBySemantic("Semantic1", {{"Unique1"}}), DMibErrorInstance("No secret matching Semantic ID: 'Semantic1' and Tag: 'Unique1'"));
					DMibExpectException(fGetBySemantic("Semantic1", {{"Shared1"}}), DMibErrorInstance("No secret matching Semantic ID: 'Semantic1' and Tag: 'Shared1'"));
				}
				fRemovePermissions(Needed);
			}
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test EnumerateSecrets Command Permissions");
				DMibExpectException(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), DMibErrorInstance("Access denied"));
				
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/EnumerateSecrets"
						
					)
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted, in this case we should get the matching ID
				DMibExpect(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}}));
				
				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test EnumerateSecrets Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));
					
					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					// As in the SemanticID case enumerate simply skips the secrets without permission and we get an empty set
					if (Needed.fs_GetKey(Permission) == "SecretsManager/Command/EnumerateSecrets")
						DMibExpectException(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), DMibErrorInstance("Access denied"));
					else
						DMibExpect(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));

					fAddPermission(Needed.fs_GetKey(Permission));
				}
				
				{
					DMibTestPath("Test EnumerateSecrets Command Permissions after permissions have been restored");
					// Check that access is permitted again
					DMibExpect(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}}));
				}
			}

			{
				// No permissions set -> Access denied
				DMibTestPath("Test SetProperties Command Permissions");
				DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note")), DMibErrorInstance("Access denied"));
				
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/SetSecretProperties"
					)
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note"));

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test SetProperties Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note")), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}

				{
					DMibTestPath("Test SetProperties Permissions details");
					// Check that access is permitted again
					fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note"));
					// Cannot set/change SemanticID without permission
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("NoMatch")), DMibErrorInstance("Access denied"));
					DMibExpectException(fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetSemanticID("NoMatch")), DMibErrorInstance("Access denied"));
					// Cannot set/change Tags without permission
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetTags({"NoMatch"})), DMibErrorInstance("Access denied"));
					DMibExpectException(fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetTags({"NoMatch"})), DMibErrorInstance("Access denied"));
					// Check that we can set SemanticID when we have permission
					fAddPermissions({"SecretsManager/Write/SemanticID/SemanticNew/Tag/Unique1", "SecretsManager/Write/SemanticID/SemanticNew/Tag/Shared1"});
					fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("SemanticNew"));
					fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("Semantic1"));
					fRemovePermissions({"SecretsManager/Write/SemanticID/SemanticNew/Tag/Unique1", "SecretsManager/Write/SemanticID/SemanticNew/Tag/Shared1"});
					// Check that we can change the tags
					fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetTags({"Shared1"}));
					fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetTags({"Shared1", "Unique1"}));

					// Check that we cannot create a new secret with out permission for both tags and semantic ID
					// Unset value are only allowed with the NoSemanticID and/or NoTag permissions
					fAddPermissions({"SecretsManager/Write/SemanticID/SemanticNew/Tag/TagNew"});
					DMibExpectException
						(
							 fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetSemanticID("SemanticNew").f_SetTags({"NoMatch"}))
							, DMibErrorInstance("Access denied")
						)
					;

					DMibExpectException
						(
							 fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetSemanticID("NoMatch").f_SetTags({"TagNew"}))
							, DMibErrorInstance("Access denied")
						)
					;
					DMibExpectException(fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetSemanticID("SemanticNew")), DMibErrorInstance("Access denied"));
					DMibExpectException(fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetTags({"TagNew"})), DMibErrorInstance("Access denied"));
					fSetProperties("New", "New", CSecretsManager::CSecretProperties{}.f_SetSemanticID("SemanticNew").f_SetTags({"TagNew"}));
					fAddPermissions({"SecretsManager/Write/NoSemanticID/Tag/TagNew", "SecretsManager/Write/SemanticID/SemanticNew/NoTag"});
					fSetProperties("New", "New1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("SemanticNew"));
					fSetProperties("New", "New2", CSecretsManager::CSecretProperties{}.f_SetTags({"TagNew"}));
					DMibExpectException(fSetProperties("New", "New3", CSecretsManager::CSecretProperties{}), DMibErrorInstance("Access denied"));
					fAddPermissions({"SecretsManager/Write/NoSemanticID/NoTag"});
					fSetProperties("New", "New4", CSecretsManager::CSecretProperties{});
				}
				fRemovePermissions(Needed);
				fRemovePermissions({"SecretsManager/Write/SemanticID/SemanticNew/Tag/TagNew"});
				fRemovePermissions({"SecretsManager/Write/NoSemanticID/Tag/TagNew", "SecretsManager/Write/SemanticID/SemanticNew/NoTag"});
				fRemovePermissions({"SecretsManager/Write/NoSemanticID/NoTag"});
			}

			{
				DMibTestPath("Test ModifyTags Command Permissions");
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {}), DMibErrorInstance("Access denied"));
				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Extra"
						, "SecretsManager/Command/ModifyTags"
					)
				;
				auto Needed2 = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Extra"
						, "SecretsManager/Command/GetSecretProperties"
					)
				;
				fAddPermissions(Needed);
				fAddPermissions(Needed2);

				// Check that access is permitted
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1"}}));

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test ModifyTags Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {}), DMibErrorInstance("Access denied"));
					DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {}, {{"Extra"}}), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}

				// Check that access is permitted again
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Extra"})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1", "Extra"}}));

				// We cannot remove all tags without NoTag permission
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Unique1", "Shared1", "Extra"}}, {}), DMibErrorInstance("Access denied"));
				fAddPermission("SecretsManager/Write/SemanticID/Semantic1/NoTag");
				fAddPermission("SecretsManager/Read/SemanticID/Semantic1/NoTag");
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {"Unique1", "Shared1", "Extra"}, {})).m_Tags, ==, (TCSet<NStr::CStr>{}));
				fRemovePermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});

				// Same here, we cannot add tags from the NoTags state without that permission
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Unique1", "Shared1"}), DMibErrorInstance("Access denied"));
				fAddPermission("SecretsManager/Write/SemanticID/Semantic1/NoTag");
				fAddPermission("SecretsManager/Read/SemanticID/Semantic1/NoTag");
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Unique1", "Shared1"})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1"}}));

				fRemovePermissionsMap(Needed);
				fRemovePermissionsMap(Needed2);
				fRemovePermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});
			}

			{
				DMibTestPath("Test SetMetadata Command Permissions");

				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/SetMetadata"
					)
				;
				fAddPermissions(Needed);

				// Check that access was granted
				fSetKeyValue("Folder1", "Name1", "Key", "Value");

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test SetMetadata Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fSetKeyValue("Folder1", "Name1", "Key", "Value"), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}
				// Check that access was granted
				fSetKeyValue("Folder1", "Name1", "Key2", "Value");

				fRemovePermissions(Needed);
			}


			{
				DMibTestPath("Test RemoveMetadata Command Permissions");

				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/RemoveMetadata"
					)
				;
				fAddPermissions(Needed);

				// Check that access was granted
				fRemoveKey("Folder1", "Name1", "Key");

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test RemoveMetadata Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fRemoveKey("Folder1", "Name1", "Key2"), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
				}
				// Check that access was granted
				fRemoveKey("Folder1", "Name1", "Key2");
				fAddPermissions({});

				fRemovePermissions(Needed);
			}

			{
				DMibTestPath("Test RemoveSecret Command Permissions");

				auto Needed = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Write/SemanticID/Semantic1/Tag/T1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/T2"
						, "SecretsManager/Command/RemoveSecret"
					)
				;
				auto NeededForTest = fg_CreateMap<CStr, CPermissionRequirements>
					(
						"SecretsManager/Read/SemanticID/Semantic1/Tag/T1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/T2"
						, "SecretsManager/Command/SetSecretProperties"
						, "SecretsManager/Command/GetSecret"
					)
				;
				fAddPermissions(NeededForTest);
				fAddPermissions(Needed);
				fSetProperties("Removable", "Name1", CSecretsManager::CSecretProperties{}.f_SetSemanticID("Semantic1").f_SetTags({"T1", "T2"}));
				fRemovePermissions({"SecretsManager/Command/SetSecretProperties"});


				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test RemoveSecret Permissions. Missing '{}' permission", Needed.fs_GetKey(Permission)));

					// Remove one, check, and add it again
					fRemovePermissions({Needed.fs_GetKey(Permission)});
					DMibExpectException(fRemoveSecret("Removable", "Name1"), DMibErrorInstance("Access denied"));
					fAddPermission(Needed.fs_GetKey(Permission));
					DMibExpect(fGetSecret("Removable", "Name1"), ==, CSecretsManager::CSecret{});
				}

				// Check that access was granted and the secret removed
				fRemoveSecret("Removable", "Name1");
				DMibExpectException(fGetSecret("Removable", "Name1"), DMibErrorInstance("No secret matching ID: 'Removable/Name1'"));

				fRemovePermissions(Needed);
				fRemovePermissions(NeededForTest);
			}

			// Reset permissions
			fAddPermissions(SecretsManagerPermissionsForTest);
			DMibExpect(*fGetProperties("Folder1", "Name1").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{}));
		};

		{
			DMibTestPath("Check Database Content");

			for (auto &Launch: SecretsManagerLaunches)
				Launch.m_Subscription->f_Destroy().f_CallSync(g_Timeout);

			// Launch SecretsManagers

			fLaunchSecretManagers();

			fSetupListen();

			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			auto SecretsManagers = Subscriptions.f_SubscribeMultiple<CSecretsManager>(nSecretsManagers);
			auto SecretsManager = SecretsManagers[0];

			//
			// Set up a number of secrets and send them to the manager
			//
			auto fGetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name) -> CSecretsManager::CSecretProperties
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;

			{
				//
				// Verify that we get the correct secrets from different folders and Names
				//
				DMibTestPath("Get Properties");

				// Verify all properties in the first secret
				auto Properties = fGetProperties("Folder1", "Name1");
				DMibExpect(*Properties.m_Secret, ==, StringSecret);
				DMibExpect(*Properties.m_UserName, ==, "UserName");
				DMibExpect(*Properties.m_URL, ==, "http://URL/");
				DMibExpect(*Properties.m_Expires, ==, NTime::CTimeConvert::fs_CreateTime(1971, 1, 1));
				DMibExpect(*Properties.m_Notes, ==, "Note");
				DMibExpect(*Properties.m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{}));
				DMibExpect(*Properties.m_Created, ==, NTime::CTimeConvert::fs_CreateTime(1972, 2, 2));
				DMibExpect(*Properties.m_SemanticID, ==, "Semantic1");
				DMibExpect(*Properties.m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));

				DMibExpect(*fGetProperties("Folder1", "Name2").m_Notes, ==, "Testing12");
				DMibExpect(*fGetProperties("Folder2", "Name1").m_Notes, ==, "Testing21");
				DMibExpect(*fGetProperties("Folder2", "Name2").m_Notes, ==, "Testing22");
			}
		};

		{
			DMibTestPath("Upload and Download");

			CTrustedSubscriptionTestHelper Subscriptions{TrustManager};
			auto SecretsManagers = Subscriptions.f_SubscribeMultiple<CSecretsManager>(nSecretsManagers);
			auto SecretsManager = SecretsManagers[0];
			auto &SecretsManagerLaunchInfo = SecretsManagerLaunches[0];

			CSecretsManager::CSecretID ID{"Folder1", "Name1"};
			CStr File1 = "TestFile1.txt";
			CStr File2 = "TestFile2.txt";
			CStr File3 = "TestFile3.txt";

			auto fGetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name) -> CSecretsManager::CSecretProperties
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;
			auto fSetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, CSecretsManager::CSecretProperties &&_Properties)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_SetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}, fg_Move(_Properties) ).f_CallSync(g_Timeout);
				}
			;
			auto fSetPropertiesNoWait = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, CSecretsManager::CSecretProperties &&_Properties) -> TCFuture<void>
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_SetSecretProperties, CSecretsManager::CSecretID{_Folder, _Name}, fg_Move(_Properties));
				}
			;
			auto fRemoveSecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name)
				{
					DMibCallActor(SecretsManager, CSecretsManager::f_RemoveSecret, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
				}
			;
			auto fRemoveSecretNoWait = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name)  -> TCFuture<void>
				{
					return DMibCallActor(SecretsManager, CSecretsManager::f_RemoveSecret, CSecretsManager::CSecretID{_Folder, _Name});
				}
			;
			auto fWriteFile = [&](CStr _FileName, CStr _Content)
				{
					CFile File;
					File.f_Open(CFile::fs_AppendPath(RootDirectory, _FileName), EFileOpen_Write | EFileOpen_ShareAll);
					File.f_Write(_Content.f_GetStr(), _Content.f_GetStrLen());
					File.f_Close();
				}
			;
			auto fAreIdentical = [&](CStr const &_FileName1, CStr const &_FileName2)
				{
					NFile::CFile File1;
					NFile::CFile File2;

					File1.f_Open(CFile::fs_AppendPath(RootDirectory, _FileName1), EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_NoLocalCache);
					File2.f_Open(CFile::fs_AppendPath(RootDirectory, _FileName2), EFileOpen_Read | EFileOpen_ShareAll | EFileOpen_NoLocalCache);

					NStream::CFilePos FileLen = File1.f_GetLength();
					if (FileLen != File2.f_GetLength())
						return false;

					NContainer::CByteVector DestData1;
					NContainer::CByteVector DestData2;
					File1.f_Read(DestData1.f_GetArray(FileLen), FileLen);
					File2.f_Read(DestData2.f_GetArray(FileLen), FileLen);

					return NMemory::fg_MemCmp(DestData1.f_GetArray(), DestData2.f_GetArray(), FileLen) == 0;
				}
			;

			auto fUpload = [&]
				(
				 	CSecretsManager::CSecretID const &_ID
				   	, CStr const &_FileName
				   	, CStr const &_BasePath
				 	, CActorSubscription &o_Subscription
				   	, NThread::CEvent *_pOpenEvent = nullptr
				   	, NThread::CEvent *_pCloseEvent = nullptr
				)
				-> TCFuture<uint32>
				{

					CDirectorySyncSend::CConfig Config(CFile::fs_AppendPath(_BasePath, _FileName));
					Config.m_FileOptions.m_fOpenStream = [pOpenEvent = _pOpenEvent, pCloseEvent = _pCloseEvent]
						(
							CStr const &_FileName
							, EDirectorySyncStreamType _FileType
							, EFileOpen _OpenFlags
							, EFileAttrib _Attributes
						)
						-> NStorage::TCUniquePointer<NStream::CBinaryStream>
						{
							TCUniquePointer<TCBinaryStreamFileDelayed<>> pFile = fg_Construct(pOpenEvent, pCloseEvent);
							pFile->f_Open(_FileName, _OpenFlags, _Attributes);
							return pFile;
						}
					;

					TCPromise<uint32> Promise;
					fg_UploadSecretFile
						(
						 	SecretsManager
						 	, Dependencies.m_DistributionManager
						 	, fg_TempCopy(_ID)
						 	, fg_Move(Config)
						 	, o_Subscription
						)
						> Promise / [Promise] (CDirectorySyncSend::CSyncResult &&_Result) mutable
						{
							Promise.f_SetResult(_Result.m_Stats.m_nSyncedFiles - 1);
						}
					;
					return Promise.f_MoveFuture();
				}
			;
			auto fDownload = [&]
				(
				 	CSecretsManager::CSecretID _ID
				 	, CStr const &_Outfile
				 	, CStr const &_BasePath
				 	, NThread::CEvent *_pOpenEvent = nullptr
				 	, CDirectorySyncReceive::EEasyConfigFlag _Flags = CDirectorySyncReceive::EEasyConfigFlag_None
				)
				-> TCFuture<uint32>
				{
					TCPromise<uint32> Promise;

					CDirectorySyncReceive::CConfig Config(CFile::fs_GetExpandedPath(_Outfile, _BasePath), _Flags);
					Config.m_FileOptions.m_fOpenStream = [_pOpenEvent]
						(
							CStr const &_FileName
							, EDirectorySyncStreamType _FileType
							, EFileOpen _OpenFlags
							, EFileAttrib _Attributes
						)
						-> NStorage::TCUniquePointer<NStream::CBinaryStream>
						{
							TCUniquePointer<TCBinaryStreamFileDelayed<>> pFile = fg_Construct(_pOpenEvent, nullptr);
							pFile->f_Open(_FileName, _OpenFlags, _Attributes);
							return pFile;
						}
					;

					fg_DownloadSecretFile(SecretsManager, fg_Move(_ID), fg_Move(Config))
						> Promise % "Failed to transfer secret file" / [=](NFile::CDirectorySyncReceive::CSyncResult &&_Result)
						{
							Promise.f_SetResult(_Result.m_Stats.m_nSyncedFiles - 1); // Subtract one for the manifest file
						}
					;
					return Promise.f_MoveFuture();
				}
			;
			auto fFindFiles = [&]
				{
					return CFile::fs_FindFiles(RootDirectory + "/SecretsManager00/SecretsManagerFiles/*", EFileAttrib_File);
				}
			;
			auto fDestination = [](int _Number)
				{
					return fg_Format("Destination{}.txt", _Number);
				}
			;

			auto fSyncFileOperations = [&](CStr const &_Command, CEJSON const _Params = "") -> TCFuture<CEJSON>
				{
					return SecretsManagerLaunchInfo.f_Test_Command(_Command, _Params);
				}
			;

			{
				fWriteFile(File1, "abcdefghijklmnopqrstuvwxyz");
				fWriteFile(File2, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
				fWriteFile(File3, "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");

				auto Properties = fGetProperties("Folder1", "Name1");
				DMibExpect(*Properties.m_Secret, ==, StringSecret);

				auto Files = fFindFiles();
				{
					CActorSubscription Subscription;
					DMibExpect(fUpload(ID, File1, RootDirectory, Subscription).f_CallSync(g_Timeout), ==, 1);
					DMibExpect((*fGetProperties("Folder1", "Name1").m_Secret).f_GetTypeID(), ==, CSecretsManager::ESecretType_File);
					DMibExpect(fDownload(ID, fDestination(1), RootDirectory).f_CallSync(g_Timeout), ==, 1);
					DMibExpect(fAreIdentical(File1, fDestination(1)), ==, true);
					Files = fFindFiles();
					DMibExpect(Files.f_GetLen(), ==, 1);
				}

				{
					// New file => transfer, remove old file, create new one
					CActorSubscription Subscription;
					DMibExpect(fUpload(ID, File2, RootDirectory, Subscription).f_CallSync(g_Timeout), ==, 1);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto Files2 = fFindFiles();
					DMibExpect(Files2.f_GetLen(), ==, 1);
					DMibExpect(Files2, !=, Files);
					Files = fg_Move(Files2);
				}

				{
					// Same file, new content => transfer, remove old file, create new one
					fWriteFile(File2, "0123456789klmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz");
					CActorSubscription Subscription;
					mint nUploaded = fUpload(ID, File2, RootDirectory, Subscription).f_CallSync(g_Timeout);
					DMibExpect(nUploaded, ==, 1);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto Files3 = fFindFiles();
					DMibExpect(Files3.f_GetLen(), ==, 1);
					DMibExpect(Files3, !=, Files);
					DMibExpect(fDownload(ID, fDestination(2), RootDirectory).f_CallSync(g_Timeout), ==, 1);
					DMibExpectTrue(fAreIdentical(File2, fDestination(2)));
					Files = fg_Move(Files3);
				}

				{
					// Upload to non-existing secret
					CActorSubscription Subscription;
					DMibExpectException
						(
							fUpload(CSecretsManager::CSecretID{"NonExisting", "NonExisting"}, File2, RootDirectory, Subscription).f_CallSync(g_Timeout)
							, DMibErrorInstance("No secret matching ID: 'NonExisting/NonExisting'")
						)
					;
				}

				{
					// Two competing uploads, first to complete wins, remove losing file
					CActorSubscription Subscription;
					NThread::CEvent Event;
					auto UploadInitializedFuture = fSyncFileOperations("UploadInitialized", File1);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fUpload(ID, File1, RootDirectory, Subscription, &Event);
					// Wait for secrets manager to handle the upload request
					UploadInitializedFuture.f_CallSync(g_Timeout);
					CActorSubscription Subscription2;
					DMibExpect(fUpload(ID, File3, RootDirectory, Subscription2).f_CallSync(g_Timeout), ==, 1);
					// Release the file1 rsync
					Event.f_SetSignaled();
					DMibExpectException
						(
							Promise.f_CallSync(g_Timeout)
							, DMibErrorInstance("The secret property in secret 'Folder1/Name1' was changed while the secret file was uploaded. Please check and upload again.")
						)
					;
					DMibExpect(fDownload(ID, fDestination(3), RootDirectory).f_CallSync(g_Timeout), ==, 1);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					DMibExpectTrue(fAreIdentical(File3, fDestination(3)));
					auto Files4 = fFindFiles();
					DMibExpect(Files4.f_GetLen(), ==, 1);
					DMibExpect(Files4, !=, Files);
					Files = Files4;
				}

				{
					DMibTestPath("Secret changed during upload");
					// Initiate upload, change the secret to a string secret, same error as above, remove file
					CActorSubscription Subscription;
					NThread::CEvent Event;
					auto UploadInitializedFuture = fSyncFileOperations("UploadInitialized", File2);
					auto UploadCompletedFuture = fSyncFileOperations("UploadCompleted", File2);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fUpload(ID, File2, RootDirectory, Subscription, &Event);
					// Wait for secrets manager to handle the upload request
					UploadInitializedFuture.f_CallSync(g_Timeout);
					fSetProperties(ID.m_Folder, ID.m_Name, CSecretsManager::CSecretProperties{}.f_SetSecret(fg_TempCopy(StringSecret)));
					// Flush file ops before checking number of files in the secret directory

					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);

					auto FilesAfterSetSecret = fFindFiles();
					DMibTest(DMibExpr(FilesAfterSetSecret.f_GetLen()) <= DMibExpr(1u) && DMibExpr(FilesAfterSetSecret) != DMibExpr(Files));
					Event.f_SetSignaled();
					// Wait for secrets manager to start the rsync
					UploadCompletedFuture.f_CallSync(g_Timeout);
					DMibExpectException
						(
							Promise.f_CallSync(g_Timeout)
							, DMibErrorInstance("The secret property in secret 'Folder1/Name1' was changed while the secret file was uploaded. Please check and upload again.")
						)
					;
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					DMibExpect(0, ==, fFindFiles().f_GetLen());
				}

				{
					// Secret deleted during upload, report error, remove downloaded file
					fSetProperties("Sacrificial", "Lamb", CSecretsManager::CSecretProperties{}.f_SetNotes("Note"));
					CActorSubscription Subscription;
					NThread::CEvent Event;
					auto UploadInitializedFuture = fSyncFileOperations("UploadInitialized", File1);
					auto UploadCompletedFuture = fSyncFileOperations("UploadCompleted", File1);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fUpload(CSecretsManager::CSecretID{"Sacrificial", "Lamb"}, File1, RootDirectory, Subscription, &Event);
					// Wait for secrets manager to handle the upload request
					UploadInitializedFuture.f_CallSync(g_Timeout);
					fRemoveSecret("Sacrificial", "Lamb");
					// Flush file ops
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					// Release the rsync
					Event.f_SetSignaled();
					// Wait for secrets manager to start the rsync
					UploadCompletedFuture.f_CallSync(g_Timeout);
					DMibExpectException
						(
							Promise.f_CallSync(g_Timeout)
							, DMibErrorInstance("Secret 'Sacrificial/Lamb' removed while the secret file was uploaded")
						)
					;
					DMibExpect(fFindFiles().f_GetLen(), ==, 0);
				}

				{
					CActorSubscription Subscription;
					fUpload(ID, File2, RootDirectory, Subscription).f_CallSync(g_Timeout);
					// Download initiated before upload, starts after upload completes, should get old file
					auto DownloadInitializedFuture = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fDownload(ID, fDestination(4), RootDirectory);
					// Wait for download to start
					DownloadInitializedFuture.f_CallSync(g_Timeout);
					auto Files5 = fFindFiles();
					CActorSubscription Subscription2;
					fUpload(ID, File3, RootDirectory, Subscription2).f_CallSync(g_Timeout);
					// Old file from first download?
					DMibExpect(Promise.f_CallSync(g_Timeout), ==, 1);
					DMibExpectTrue(fAreIdentical(File2, fDestination(4)));
					// New file from the download after the upload
					fDownload(ID, fDestination(12), RootDirectory).f_CallSync(g_Timeout);
					DMibExpectTrue(fAreIdentical(File3, fDestination(12)));

					auto Files7 = fFindFiles();
					DMibExpect(Files5.f_GetLen(), ==, 1);
					DMibExpect(Files7.f_GetLen(), ==, 1);
					DMibExpect(Files7, !=, Files5);
				}

				{
					DMibTestPath("Overwrite on download");
					// Check that file is not overwritten
					DMibExpectExceptionType(fDownload(ID, fDestination(12), RootDirectory).f_CallSync(g_Timeout), NException::CException);
					// Check that file is overwritten
					fDownload(ID, fDestination(12), RootDirectory, nullptr, CDirectorySyncReceive::EEasyConfigFlag_AllowOverwrite).f_CallSync(g_Timeout);
					DMibExpectTrue(fAreIdentical(File3, fDestination(12)));
				}

				{
					DMibTestPath("Secret changed during download");
					// Download initiated before secret change, get the old file
					NThread::CEvent Event;
					auto DownloadInitializedFuture = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					auto DownloadCompletedFuture = fSyncFileOperations("DownloadCompleted", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fDownload(ID, fDestination(5), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture.f_CallSync(g_Timeout);
					auto SecretFuture = fSetPropertiesNoWait(ID.m_Folder, ID.m_Name, CSecretsManager::CSecretProperties{}.f_SetSecret(fg_TempCopy(StringSecret)));
					// Release the download
					Event.f_SetSignaled();
					DMibExpect(Promise.f_CallSync(g_Timeout), ==, 1);
					DMibExpectTrue(fAreIdentical(File3, fDestination(5)));
					// Wait for completion and flush file ops
					DownloadCompletedFuture.f_CallSync(g_Timeout);
					SecretFuture.f_CallSync(g_Timeout);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto Files8 = fFindFiles();
					DMibExpect(Files8.f_GetLen(), ==, 0);
				}
				
				{
					CActorSubscription Subscription;
					fUpload(ID, File2, RootDirectory, Subscription).f_CallSync(g_Timeout);
					DMibTestPath("Secret removed during download");
					// Download initiated before remove, starts after secret removal
					NThread::CEvent Event;
					auto DownloadInitializedFuture = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					auto DownloadCompletedFuture = fSyncFileOperations("DownloadCompleted", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Promise = fDownload(ID, fDestination(6), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture.f_CallSync(g_Timeout);
					auto RemoveFuture = fRemoveSecretNoWait(ID.m_Folder, ID.m_Name);
					// Flush file ops
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					// Release the download
					Event.f_SetSignaled();
					DMibExpect(Promise.f_CallSync(g_Timeout), ==, 1);
					DMibExpectTrue(fAreIdentical(File2, fDestination(6)));
					// Wait for completion and flush file ops
					DownloadCompletedFuture.f_CallSync(g_Timeout);
					RemoveFuture.f_CallSync(g_Timeout);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto Files9 = fFindFiles();
					DMibExpect(Files9.f_GetLen(), ==, 0);
				}

				{
					CActorSubscription Subscription;
					fSetProperties(ID.m_Folder, ID.m_Name, CSecretsManager::CSecretProperties{}.f_SetSecret(fg_TempCopy(StringSecret)));
					fUpload(ID, File2, RootDirectory, Subscription).f_CallSync(g_Timeout);
					DMibTestPath("Secret changed during download - multiple downloaders");
					// Downloads initiated before secret change, get the old file
					NThread::CEvent Event;
					auto DownloadInitializedFuture1 = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Future1 = fDownload(ID, fDestination(7), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture1.f_CallSync(g_Timeout);
					auto DownloadInitializedFuture2 = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Future2 = fDownload(ID, fDestination(8), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture2.f_CallSync(g_Timeout);
					auto DownloadInitializedFuture3 = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Future3 = fDownload(ID, fDestination(9), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture3.f_CallSync(g_Timeout);
					auto DownloadInitializedFuture4 = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					auto Future4 = fDownload(ID, fDestination(10), RootDirectory, &Event);
					// Wait for download to start
					DownloadInitializedFuture4.f_CallSync(g_Timeout);
					auto DownloadCompletedFuture = fSyncFileOperations("DownloadCompleted", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);

					auto SecretFuture = fSetPropertiesNoWait(ID.m_Folder, ID.m_Name, CSecretsManager::CSecretProperties{}.f_SetSecret(fg_TempCopy(StringSecret)));
					// Release the download
					Event.f_SetSignaled();
					DMibExpect(Future1.f_CallSync(g_Timeout), ==, 1);
					DMibExpect(Future2.f_CallSync(g_Timeout), ==, 1);
					DMibExpect(Future3.f_CallSync(g_Timeout), ==, 1);
					DMibExpect(Future4.f_CallSync(g_Timeout), ==, 1);
					DMibExpectTrue(fAreIdentical(File2, fDestination(7)));
					DMibExpectTrue(fAreIdentical(File2, fDestination(8)));
					DMibExpectTrue(fAreIdentical(File2, fDestination(9)));
					DMibExpectTrue(fAreIdentical(File2, fDestination(10)));
					// Wait for completion and flush file ops
					DownloadCompletedFuture.f_CallSync(g_Timeout);
					SecretFuture.f_CallSync(g_Timeout);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto Files10 = fFindFiles();
					DMibExpect(Files10.f_GetLen(), ==, 0);
				}

				{
					CActorSubscription UploadSubscription;
					fUpload(ID, File2, RootDirectory, UploadSubscription).f_CallSync(g_Timeout);
					DMibTestPath("Keep secrets manager alive until last pending delete has completed");
					fSyncFileOperations("DelayDelete") > fg_DiscardResult();
					auto UploadCompletedFuture = fSyncFileOperations("UploadCompleted", File3);
					auto DownloadInitializedFuture = fSyncFileOperations("DownloadInitialized", ID.m_Name);
					auto DestroyWaitingForCanDestroyFuture = fSyncFileOperations("DestroyWaitingForCanDestroy", ID.m_Name);
					fSyncFileOperations("PreviousCommandCompleted").f_CallSync(g_Timeout);
					fSyncFileOperations("SyncFileOperations").f_CallSync(g_Timeout);
					auto FilesPreDownload = fFindFiles();
					DMibExpect(FilesPreDownload.f_GetLen(), ==, 1);
					NThread::CEvent Event;
					auto DownloadFuture = fDownload(ID, fDestination(11), RootDirectory, &Event);
					// Wait for download to start, now File2 is reserved
					DownloadInitializedFuture.f_CallSync(g_Timeout);
					// Upload a new file
					CActorSubscription UploadSubscription2;
					fUpload(ID, File3, RootDirectory, UploadSubscription2).f_CallSync(g_Timeout);
					UploadCompletedFuture.f_CallSync(g_Timeout);
					auto FilesPostUpload = fFindFiles();
					DMibExpect(FilesPostUpload.f_GetLen(), ==, 2);
					Event.f_SetSignaled();
					DownloadFuture.f_CallSync(g_Timeout);
					// Now the download is no longer reserving the file and
					// Kill off the Secrets Manager
					TCSharedPointer<TCAtomic<bool>> pDestroyFinished = fg_Construct(false);
					TCSharedPointer<NThread::CEvent> pDestroyFinishedEvent = fg_Construct();
					auto Subscription = fg_Move(SecretsManagerLaunches[0].m_Subscription);
					Subscription ->f_Destroy() > [=](auto &&)
						{
							pDestroyFinished->f_Exchange(true);
							pDestroyFinishedEvent->f_SetSignaled();
						}
					;
					DestroyWaitingForCanDestroyFuture.f_CallSync(g_Timeout);

					DMibExpectFalse(pDestroyFinished->f_Load());
					try
					{
						fSyncFileOperations("ReleaseDelete").f_CallSync(g_Timeout);
					}
					catch (NException::CException const &_Exception)
					{
						DMibConOut("_Exception: {}\n", _Exception);
					}
					DMibExpectFalse(pDestroyFinishedEvent->f_WaitTimeout(g_Timeout))(NTest::ETest_FailAndStop);
					auto FilesPostDestroy = fFindFiles();
					DMibExpect(FilesPostDestroy.f_GetLen(), ==, 1);
				}
			}
		}
	}
};

DMibTestRegister(CSecretsManager_Tests, Malterlib::Cloud);
