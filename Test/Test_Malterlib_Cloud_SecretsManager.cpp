
#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Cloud/SecretsManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Test/Exception>
#include <Mib/Cloud/App/SecretsManager>

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
using namespace NMib::NEncoding;
using namespace NMib::NStorage;

#define DTestSecretsManagerEnableLogging 0

static fp64 g_Timeout = 600.0;

class CSecretsManager_Tests : public NMib::NTest::CTest
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
			CStr RootDirectory = ProgramDirectory + "/SecretsTests";
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
			Security.m_AllowedIncomingConnectionNamespaces.f_Insert(CSecretsManager::mc_pDefaultNamespace);
			Dependencies.m_DistributionManager(&CActorDistributionManager::f_SetSecurity, Security).f_CallSync(g_Timeout);
			
			TCActor<CDistributedApp_LaunchHelper> LaunchHelper = fg_ConstructActor<CDistributedApp_LaunchHelper>(Dependencies, DTestSecretsManagerEnableLogging);
			auto Cleanup = g_OnScopeExit > [&]
				{
					LaunchHelper->f_BlockDestroy();
				}
			;

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
					g_Dispatch(FileActor) > [=]
						{
							CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", i);
							CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
							CFile::fs_CreateDirectory(SecretsManagerDirectory);
							CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/SecretsManager", SecretsManagerDirectory, nullptr);
						}
						> SecretsManagerLaunchesResults.f_AddResult()
					;
				}
				fg_CombineResults(SecretsManagerLaunchesResults.f_GetResults().f_CallSync());
			}

			// Launch SecretsManagers
			TCActorResultVector<CDistributedApp_LaunchInfo> SecretsManagerLaunchesResults;
			
			for (mint i = 0; i < nSecretsManagers; ++i)
			{
				CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", i);
				CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
				LaunchHelper(&CDistributedApp_LaunchHelper::f_LaunchInProcess, SecretsManagerName, SecretsManagerDirectory, &fg_ConstructApp_SecretsManager)
					> SecretsManagerLaunchesResults.f_AddResult()
				;
			}
			
			TCVector<CDistributedApp_LaunchInfo> SecretsManagerLaunches;
			for (auto &LaunchResult : SecretsManagerLaunchesResults.f_GetResults().f_CallSync(g_Timeout))
				SecretsManagerLaunches.f_Insert(fg_Move(*LaunchResult));

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
			{
				TCActorResultVector<void> ListenResults;
				mint iSecretsManager = 0;
				for (auto &SecretsManager : SecretsManagerLaunches)
				{
					CStr SecretsManagerName = fg_Format("SecretsManager{sf0,sl2}", iSecretsManager);
					CStr SecretsManagerDirectory = RootDirectory + "/" + SecretsManagerName;
					
					AllSecretsManagerHosts[SecretsManager.m_HostID];
					auto &SecretsManagerInfo = AllSecretsManagers[SecretsManager.m_HostID];
					SecretsManagerInfo.m_pTrustInterface = SecretsManager.m_pTrustInterface;
					SecretsManagerInfo.m_Address.m_URL = fg_Format("wss://[UNIX(777):{}/SecretsManagerTest.sock]/", SecretsManagerDirectory);
					DMibCallActor(*SecretsManager.m_pTrustInterface, CDistributedActorTrustManagerInterface::f_AddListen, SecretsManagerInfo.m_Address) > ListenResults.f_AddResult();
					++iSecretsManager;
				}
				fg_CombineResults(ListenResults.f_GetResults().f_CallSync(g_Timeout));
			}

			TCActorResultVector<void> SetupTrustResults;
			
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
						, "com.malterlib/Cloud/SecretsManagerCoordination"
						, TrustSecretsManagers
					) 
					> SetupTrustResults.f_AddResult()
				;
#endif
				
				DMibCallActor
					(
						TrustManager
						, CDistributedActorTrustManager::f_AllowHostsForNamespace
						, CSecretsManager::mc_pDefaultNamespace
						, fg_CreateSet<CStr>(SecretsManagerHostID)
					)				
					> SetupTrustResults.f_AddResult()
				;
					
				for (auto &SecretsManagerInner : AllSecretsManagers)
				{
					CStr SecretsManagerHostIDInner = SecretsManagerInner.f_GetHostID();
					if (SecretsManagerHostIDInner == SecretsManagerHostID)
						continue;
					
					auto pSecretsManagerTrustInner = SecretsManagerInner.m_pTrustInterface;
					
					TCContinuation<void> Continuation;
					DMibCallActor(SecretsManagerTrust, CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket, SecretsManager.m_Address, nullptr)
						> Continuation / [=](CDistributedActorTrustManagerInterface::CTrustGenerateConnectionTicketResult &&_Ticket)
						{
							auto &SecretsManagerTrustInner = *pSecretsManagerTrustInner;
							DMibCallActor(SecretsManagerTrustInner, CDistributedActorTrustManagerInterface::f_AddClientConnection, _Ticket.m_Ticket, g_Timeout, -1) > Continuation.f_ReceiveAny();
						}
					;
					Continuation.f_Dispatch() > SetupTrustResults.f_AddResult();
				}
			}
			
			auto SecretsManagers = Subscriptions.f_SubscribeMultiple<CSecretsManager>(nSecretsManagers);
			auto SecretsManager = SecretsManagers[0];

			//
			// Set up a number of secrets and send them to the manager
			//
			CSecretsManager::CSecretID SecretID11{"Folder1", "Name1"};
			CSecretsManager::CSecretID SecretID12{"Folder1", "Name2"};
			CSecretsManager::CSecretID SecretID21{"Folder2", "Name1"};
			CSecretsManager::CSecretID SecretID22{"Folder2", "Name2"};

			CSecretsManager::CSecret StringSecret{"Secret1"};
			CSecretsManager::CSecret ByteVectorSecret{CSecureByteVector{(uint8 const *)"Secret2", 7}};
			CSecretsManager::CSecret FileSecret{CSecretsManager::CFileTag{}};
			
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, fg_TempCopy(SecretID11)
					, CSecretsManager::CSecretProperties{}
					.f_Secret(fg_TempCopy(StringSecret))
					.f_UserName("UserName")
					.f_URL("http://URL/")
					.f_Expires(NTime::CTimeConvert::fs_CreateTime(1971, 1, 1))
					.f_Notes("Testing11")
					.f_Metadata("Key", "Value")
					.f_Created(NTime::CTimeConvert::fs_CreateTime(1972, 2, 2))
					.f_Modified(NTime::CTimeConvert::fs_CreateTime(1973, 3, 3))
					.f_SemanticID("Semantic1")
					.f_Tags({"Shared1", "Unique1"})
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder1", "Name2"}
					, CSecretsManager::CSecretProperties{}.f_Notes("Testing12").f_Secret(fg_TempCopy(ByteVectorSecret)).f_Tags({"Shared1", "Shared2", "Unique2"}).f_SemanticID("Semantic2")
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder2", "Name1"}
					, CSecretsManager::CSecretProperties{}.f_Notes("Testing21").f_Secret(fg_TempCopy(FileSecret)).f_Tags({"Shared2"}).f_SemanticID("Semantic3")
				)
				.f_CallSync(g_Timeout)
			;
			DMibCallActor
				(
					SecretsManager
					, CSecretsManager::f_SetSecretProperties
					, CSecretsManager::CSecretID{"Folder2", "Name2"}
					, CSecretsManager::CSecretProperties{}.f_Notes("Testing22").f_Metadata("Key1", "Value1")
				)
				.f_CallSync(g_Timeout)
			;

			
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
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Secret, ==, StringSecret);
				DMibExpect(*fGetProperties("Folder1", "Name1").m_UserName, ==, "UserName");
				DMibExpect(*fGetProperties("Folder1", "Name1").m_URL, ==, "http://URL/");
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Expires, ==, NTime::CTimeConvert::fs_CreateTime(1971, 1, 1));
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Notes, ==, "Testing11");
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key", "Value"}}));
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Created, ==, NTime::CTimeConvert::fs_CreateTime(1972, 2, 2));
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Modified, ==, NTime::CTimeConvert::fs_CreateTime(1973, 3, 3));
				DMibExpect(*fGetProperties("Folder1", "Name1").m_SemanticID, ==, "Semantic1");
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				
				DMibExpect(*fGetProperties("Folder1", "Name2").m_Notes, ==, "Testing12");
				DMibExpect(*fGetProperties("Folder2", "Name1").m_Notes, ==, "Testing21");
				DMibExpect(*fGetProperties("Folder2", "Name2").m_Notes, ==, "Testing22");

				DMibExpectException(fGetProperties("Folder1", "NoMatch"), DMibErrorInstance("SecretID does not exist"));
				DMibExpectException(fGetProperties("NoMatch", "Name1"), DMibErrorInstance("SecretID does not exist"));
			}
			
			{
				DMibTestPath("Get Secrets");

				auto fGetSecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name) -> CSecretsManager::CSecret
					{
						return DMibCallActor(SecretsManager, CSecretsManager::f_GetSecret, CSecretsManager::CSecretID{_Folder, _Name}).f_CallSync(g_Timeout);
					}
				;
				DMibExpect(fGetSecret("Folder1", "Name1"), ==, StringSecret);
				DMibExpect(fGetSecret("Folder1", "Name2"), ==, ByteVectorSecret);
				DMibExpect(fGetSecret("Folder2", "Name1"), ==, FileSecret);

				DMibExpectException(fGetSecret("Folder2", "Name2"), DMibErrorInstance("No secret set"));
				DMibExpectException(fGetSecret("NoMatch", "Name2"), DMibErrorInstance("SecretID does not exist"));
				DMibExpectException(fGetSecret("Folder1", "NoMatch"), DMibErrorInstance("SecretID does not exist"));

			}
			
			{
				DMibTestPath("Test ModifyTags");

				// Verify that we have the tags that we inserted in the secret properties
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Unique1"}}));
				DMibExpect(*fGetProperties("Folder1", "Name2").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared1", "Shared2", "Unique2"}}));
				DMibExpect(*fGetProperties("Folder2", "Name1").m_Tags, ==, (TCSet<NStr::CStr>{{"Shared2"}}));
				
				auto fAddTagsAndGetProperties = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, TCSet<NStr::CStr> _RemoveTags, NContainer::TCSet<NStr::CStr> _AddTags)
					-> CSecretsManager::CSecretProperties
					{
						DMibCallActor(SecretsManager, CSecretsManager::f_ModifyTags, CSecretsManager::CSecretID{_Folder, _Name}, _RemoveTags, _AddTags).f_CallSync(g_Timeout);
						return fGetProperties(_Folder, _Name);
					}
				;

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
				auto fGetNonExistingSecret = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name)
					{
						DMibCallActor(SecretsManager, CSecretsManager::f_ModifyTags, CSecretsManager::CSecretID{_Folder, _Name}, TCSet<NStr::CStr>{}, TCSet<NStr::CStr>{})
							.f_CallSync(g_Timeout)
						;
					}
				;
				DMibExpectException(fGetNonExistingSecret("NoMatch", "Name1"), DMibErrorInstance("SecretID does not exist"));
				DMibExpectException(fGetNonExistingSecret("Folder1", "NoMatch"), DMibErrorInstance("SecretID does not exist"));
			}
			
			{
				DMibTestPath("Test Enumeration");
				auto fEnumerateFor = [&](TCOptional<NStr::CStrSecure> _ID, TCSet<NStr::CStr> _Tags) -> TCSet<CSecretsManager::CSecretID>
					{
						return DMibCallActor(SecretsManager, CSecretsManager::f_EnumerateSecrets, _ID, _Tags).f_CallSync(g_Timeout);
					}
				;
				DMibExpect(fEnumerateFor({},              {}),            ==, (TCSet<CSecretsManager::CSecretID>{SecretID11, SecretID12, SecretID21, SecretID22}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {}),            ==, (TCSet<CSecretsManager::CSecretID>{SecretID11}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {{"Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{SecretID11}));
				DMibExpect(fEnumerateFor({{"Semantic1"}}, {{"NoMatch"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));
				DMibExpect(fEnumerateFor({},              {{"Shared1"}}), ==, (TCSet<CSecretsManager::CSecretID>{SecretID11, SecretID12}));
				DMibExpect(fEnumerateFor({},              {{"Shared2"}}), ==, (TCSet<CSecretsManager::CSecretID>{SecretID21, SecretID12}));
				DMibExpect(fEnumerateFor({},              {{"NoMatch"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));
				DMibExpect(fEnumerateFor({{"NoMatch"}},   {}),            ==, (TCSet<CSecretsManager::CSecretID>{}));
			}
			
			{
				DMibTestPath("Test metadata");
				

				auto fSetKeyValueNoGet = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key, CEJSON const &_Value)
					{
						DMibCallActor(SecretsManager, CSecretsManager::f_SetMetadata, CSecretsManager::CSecretID{_Folder, _Name}, fg_TempCopy(_Key), fg_TempCopy(_Value))
							.f_CallSync(g_Timeout)
						;
					}
				;
				
				auto fSetKeyValue = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key, CEJSON const &_Value) -> CSecretsManager::CSecretProperties
					{
						fSetKeyValueNoGet(_Folder, _Name, _Key, _Value);
						return fGetProperties(_Folder, _Name);
					}
				;
				
				auto fRemoveKeyNoGet = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key)
					{
						DMibCallActor(SecretsManager, CSecretsManager::f_RemoveMetadata, CSecretsManager::CSecretID{_Folder, _Name}, fg_TempCopy(_Key)).f_CallSync(g_Timeout);
					}
				;

				auto fRemoveKey = [&](NStr::CStr const &_Folder, NStr::CStr const &_Name, NStr::CStr const &_Key) -> CSecretsManager::CSecretProperties
					{
						fRemoveKeyNoGet(_Folder, _Name, _Key);
						return fGetProperties(_Folder, _Name);
					}
				;
				
				// Verify original value
				DMibExpect(*fGetProperties("Folder2", "Name2").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key1", "Value1"}}));

				DMibExpect(*fSetKeyValue("Folder2", "Name2", "Key2", "Value2").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key1", "Value1"}, {"Key2", "Value2"}}));
				DMibExpect(*fSetKeyValue("Folder2", "Name1", "Key3", "Value3").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key3", "Value3"}}));
				DMibExpectException(fSetKeyValueNoGet("Folder1", "NonExisting", "Key3", {"Value3"}), DMibErrorInstance("SecretID does not exist"));

				DMibExpect(*fRemoveKey("Folder2", "Name2", "Key1").m_Metadata, ==, (TCMap<NStr::CStrSecure, CEJSON>{{"Key2", "Value2"}}));
				DMibExpectException(fRemoveKeyNoGet("Folder1", "NoMatch", "Key3"), DMibErrorInstance("SecretID does not exist"));
			}
		};
	}
};

DMibTestRegister(CSecretsManager_Tests, Malterlib::Cloud);
