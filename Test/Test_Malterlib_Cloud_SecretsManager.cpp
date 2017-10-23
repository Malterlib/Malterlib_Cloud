
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
			TCSet<CStr> SecretsManagerPermissionsForTest = fg_CreateSet<CStr>("SecretsManager/CommandAll", "SecretsManager/*/*/*");

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
							//CFile::fs_DiffCopyFileOrDirectory(ProgramDirectory + "/TestApps/SecretsManager", SecretsManagerDirectory, nullptr);
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
				
				DMibCallActor
					(
						SecretsManagerTrust
						, CDistributedActorTrustManagerInterface::f_AddHostPermissions
						, TestHostID
						, SecretsManagerPermissionsForTest
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
			CSecretsManager::CSecret StringSecret{"Secret1"};
			CSecretsManager::CSecret ByteVectorSecret{CSecureByteVector{(uint8 const *)"Secret2", 7}};
			CSecretsManager::CSecret FileSecret{CSecretsManager::CFileTag{}};
			
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
					DMibCallActor
						(
							SecretsManager
							, CSecretsManager::f_SetSecretProperties
							, CSecretsManager::CSecretID{"Test", "Test1"}
							, CSecretsManager::CSecretProperties{}
						 )
						.f_CallSync(g_Timeout)
					;
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
							DMibTestPath("Test SetProperties - only seting Secret");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetSecret(CSecretsManager::CSecret{"newtext"}));
						}
						{
							DMibTestPath("Test SetProperties - only seting Username");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetUserName("NewUsername"));
						}
						{
							DMibTestPath("Test SetProperties - only seting URL");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetURL("NewURL"));
						}
						{
							DMibTestPath("Test SetProperties - only seting Expires");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetExpires(NTime::CTimeConvert::fs_CreateTime(1977, 7, 7)));
						}
						{
							DMibTestPath("Test SetProperties - only seting Created");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetCreated(NTime::CTimeConvert::fs_CreateTime(1988, 8, 8)));
						}
						{
							DMibTestPath("Test SetProperties - only seting Modified");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetModified(NTime::CTimeConvert::fs_CreateTime(1999, 9, 9)));
						}
						{
							DMibTestPath("Test SetProperties - only seting Notes");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetNotes("NewNote"));
						}
						{
							DMibTestPath("Test SetProperties - only seting Metadata");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetMetadata("c", "d"));
						}
						{
							DMibTestPath("Test SetProperties - only seting SemanticID");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetSemanticID("NewSemanticID"));
						}
						{
							DMibTestPath("Test SetProperties - only seting Tags");
							fSetPropertiesTestUntouched("Test", "Test2", CSecretsManager::CSecretProperties{}.f_SetTags({"NewTag1", "NewTag2"}));
						}
					}
				}
			}

			
			auto fAddPermissions = [&](TCSet<NStr::CStr> const &_Permissions)
				{
					for (auto &SecretsManager : AllSecretsManagers)
					{
						auto pSecretsManagerTrust = SecretsManager.m_pTrustInterface;
						auto &SecretsManagerTrust = *pSecretsManagerTrust;
						
						DMibCallActor
							(
								SecretsManagerTrust
								, CDistributedActorTrustManagerInterface::f_AddHostPermissions
								, TestHostID
								, _Permissions
							)
							.f_CallSync(g_Timeout)
						;
					}
				}
			;
			
			auto fRemovePermissions = [&](TCSet<NStr::CStr> const &_Permissions)
				{
					for (auto &SecretsManager : AllSecretsManagers)
					{
						auto pSecretsManagerTrust = SecretsManager.m_pTrustInterface;
						auto &SecretsManagerTrust = *pSecretsManagerTrust;
						
						DMibCallActor
							(
								SecretsManagerTrust
								, CDistributedActorTrustManagerInterface::f_RemoveHostPermissions
								, TestHostID
								, _Permissions
							)
							.f_CallSync(g_Timeout)
						;
					}
				}
			;

			// So far we have used the Wildcard and All permissions. Remove them so we can test more fine grained permissions.
			fRemovePermissions(SecretsManagerPermissionsForTest);
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test GetProperties Command Permissions");
				DMibExpectException(fGetProperties("Folder1", "Name1"), DMibErrorInstance("Access denied"));
				
				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/GetSecretProperties"
						
					}
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(*fGetProperties("Folder1", "Name1").m_Secret, ==, StringSecret);

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetProperties Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fGetProperties("Folder1", "Name1"), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
				}

				// Check that access is permitted again
				DMibExpect(*fGetProperties("Folder1", "Name1").m_UserName, ==, "UserName");

				fRemovePermissions(Needed);
			}
			
			{
				// No permissions set -> Access denied
				DMibTestPath("Test GetSecret Command Permissions");
				DMibExpectException(fGetSecret("Folder1", "Name1"), DMibErrorInstance("Access denied"));
				
				TCSet<NStr::CStr> Needed{"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1", "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1", "SecretsManager/Command/GetSecret"};
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(fGetSecret("Folder1", "Name1"), ==, StringSecret);

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetSecret Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fGetSecret("Folder1", "Name1"), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
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
				
				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/GetSecretBySemanticID"
					}
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				DMibExpect(fGetBySemantic("Semantic1", {{"Shared1", "Unique1"}}), ==, StringSecret);
				
				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test GetSecretBySemanticID Permissions. Missing '{}' permission", Permission));
					
					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					// GetBySemanticID and EnumerateSecrets simply skips the secrets one hasn't permission to see so here
					// we can either get an access denied exception or a no secret exception
					if (Permission == "SecretsManager/Command/GetSecretBySemanticID")
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
					fAddPermissions({Permission});
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
				
				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/EnumerateSecrets"
						
					}
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted, in this case we should get the matching ID
				DMibExpect(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{{"Folder1", "Name1"}}));
				
				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test EnumerateSecrets Permissions. Missing '{}' permission", Permission));
					
					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					// As in the SemanticID case enumerate simply skips the secrets without permission and we get an empty set
					if (Permission == "SecretsManager/Command/EnumerateSecrets")
						DMibExpectException(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), DMibErrorInstance("Access denied"));
					else
						DMibExpect(fEnumerateFor({"Semantic1"}, {{"Shared1", "Unique1"}}), ==, (TCSet<CSecretsManager::CSecretID>{}));

					fAddPermissions({Permission});
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
				
				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/SetSecretProperties"
					}
				;
				// Add all permissions we need
				fAddPermissions(Needed);
				
				// Check that access is permitted
				fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note"));

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test SetProperties Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fSetProperties("Folder1", "Name1", CSecretsManager::CSecretProperties{}.f_SetNotes("Note")), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
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
				DMibTestPath("Test ModifyTags Commnd Permissions");
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {}), DMibErrorInstance("Access denied"));
				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Extra"
						, "SecretsManager/Command/ModifyTags"
					}
				;
				TCSet<NStr::CStr> Needed2
					{
						"SecretsManager/Read/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Read/SemanticID/Semantic1/Tag/Extra"
						, "SecretsManager/Command/GetSecretProperties"
					}
				;
				fAddPermissions(Needed);
				fAddPermissions(Needed2);

				// Check that access is permitted
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1"}}));

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test ModifyTags Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Extra"}}, {}), DMibErrorInstance("Access denied"));
					DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {}, {{"Extra"}}), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
				}

				// Check that access is permitted again
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Extra"})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1", "Extra"}}));

				// We cannot remove all tags without NoTag permission
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {{"Unique1", "Shared1", "Extra"}}, {}), DMibErrorInstance("Access denied"));
				fAddPermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {"Unique1", "Shared1", "Extra"}, {})).m_Tags, ==, (TCSet<NStr::CStr>{}));
				fRemovePermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});

				// Same here, we cannot add tags from the NoTags state without that permission
				DMibExpectException(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Unique1", "Shared1"}), DMibErrorInstance("Access denied"));
				fAddPermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});
				DMibExpect(*(fAddTagsAndGetProperties("Folder1", "Name1", {}, {"Unique1", "Shared1"})).m_Tags, ==, (TCSet<NStr::CStr>{{"Unique1", "Shared1"}}));

				fRemovePermissions(Needed);
				fRemovePermissions(Needed2);
				fRemovePermissions({"SecretsManager/Write/SemanticID/Semantic1/NoTag", "SecretsManager/Read/SemanticID/Semantic1/NoTag"});
			}
			
			{
				DMibTestPath("Test SetMetadata Command Permissions");

				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/SetMetadata"
					}
				;
				fAddPermissions(Needed);

				// Check that access was granted
				fSetKeyValue("Folder1", "Name1", "Key", "Value");

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test SetMetadata Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fSetKeyValue("Folder1", "Name1", "Key", "Value"), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
				}
				// Check that access was granted
				fSetKeyValue("Folder1", "Name1", "Key2", "Value");

				fRemovePermissions(Needed);
			}

			{
				DMibTestPath("Test RemoveMetadata Command Permissions");

				TCSet<NStr::CStr> Needed
					{
						"SecretsManager/Write/SemanticID/Semantic1/Tag/Shared1"
						, "SecretsManager/Write/SemanticID/Semantic1/Tag/Unique1"
						, "SecretsManager/Command/RemoveMetadata"
					}
				;
				fAddPermissions(Needed);

				// Check that access was granted
				fRemoveKey("Folder1", "Name1", "Key");

				for (auto const &Permission : Needed)
				{
					DMibTestPath(fg_Format("Test RemoveMetadata Permissions. Missing '{}' permission", Permission));

					// Remove one, check, and add it again
					fRemovePermissions({Permission});
					DMibExpectException(fRemoveKey("Folder1", "Name1", "Key"), DMibErrorInstance("Access denied"));
					fAddPermissions({Permission});
				}
				// Check that access was granted
				fRemoveKey("Folder1", "Name1", "Key2");

				fRemovePermissions(Needed);
			}
		};
	}
};

DMibTestRegister(CSecretsManager_Tests, Malterlib::Cloud);
