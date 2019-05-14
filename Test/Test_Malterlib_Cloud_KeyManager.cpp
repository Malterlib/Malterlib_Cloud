
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Concurrency/DistributedActorTestHelpers>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Test/Exception>
#include <Mib/Container/Vector>

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NCloud;
using namespace NMib::NFile;
using namespace NMib::NStr;

class CKeyManager_Tests : public NMib::NTest::CTest
{
public:
	
	struct CKeyManagerServerDatabaseImpl : public ICKeyManagerServerDatabase
	{
		static constexpr bool mc_bAllowInternalAccess = true;
		
		CDatabase m_Database;
		
		TCFuture<void> f_Initialize()
		{
			return fg_Explicit();
		}
		
		TCFuture<void> f_WriteDatabase(CDatabase const &_Database)
		{
			m_Database = _Database;
			return fg_Explicit();
		}
		
		TCFuture<CDatabase> f_ReadDatabase()
		{
			return fg_Explicit(m_Database);
		}
	};
	
	void f_TestCloudKeyManager
		(
			NMib::NConcurrency::TCActor<ICKeyManagerServerDatabase> const &_Database
			, CDistributedActorTestHelperCombined &_TestHelper
			, CDistributedActorTestHelperCombined &_TestHelper2
		)
	{
		CKeyManagerServerConfig Config;
		Config.m_DatabaseActor = _Database;
		Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(60.0);
		//Config.m_PublicKeysForAllKeyManagers = ; TODO
		
		TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>(Config, fg_GetDistributionManager());
		
		auto Subscription = _TestHelper.f_Subscribe("com.malterlib/Cloud/KeyManager");
		TCDistributedActor<CKeyManager> KeyManager = _TestHelper.f_GetRemoteActor<CKeyManager>(Subscription);
		
		CSymmetricKey Key0 = KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_CallSync(60.0);
		CSymmetricKey Key1 = KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_CallSync(60.0);
		auto HostID1 = _TestHelper.f_GetClientHostID();
		
		DMibExpect(Key0, !=, Key1);
		auto Database = Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
		DMibExpect(Key0, ==, Database.m_Clients[HostID1].m_Keys["TestKey0"]);
		DMibExpect(Key1, ==, Database.m_Clients[HostID1].m_Keys["TestKey1"]);
		
		CStr Subscription2 = _TestHelper2.f_Subscribe("com.malterlib/Cloud/KeyManager");

		auto HostID2 = _TestHelper2.f_GetClientHostID();
		TCDistributedActor<CKeyManager> KeyManager2 = _TestHelper2.f_GetRemoteActor<CKeyManager>(Subscription2);

		CSymmetricKey SecondKey0 = KeyManager2.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_CallSync(60.0);
		CSymmetricKey SecondKey1 = KeyManager2.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_CallSync(60.0);
		
		DMibExpect(SecondKey0, !=, SecondKey1); 
		DMibExpect(SecondKey0, !=, Key0); 
		DMibExpect(SecondKey1, !=, Key1);
		Database = Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
		DMibExpect(SecondKey0, ==, Database.m_Clients[HostID2].m_Keys["TestKey0"]);
		DMibExpect(SecondKey1, ==, Database.m_Clients[HostID2].m_Keys["TestKey1"]);
	}
	
	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			uint16 Port = 31399;
			CDistributedActorTestHelperCombined TestHelper{Port};
			TestHelper.f_Init();
			
			CDistributedActorTestHelperCombined TestHelper2{Port};
			TestHelper2.f_InitClient(TestHelper);
			
			f_TestCloudKeyManager(fg_ConstructActor<CKeyManagerServerDatabaseImpl>(), TestHelper, TestHelper2);
		};
		DMibTestSuite("EncryptedFile")
		{
			uint16 Port = 31400;
			CStr DatabasePath = CFile::fs_GetProgramDirectory() + "/EncryptedFile";
			CStrSecure Password = "Password";
			
			CDistributedActorTestHelperCombined TestHelper{Port};
			TestHelper.f_Init();
			
			CDistributedActorTestHelperCombined TestHelper2{Port};
			TestHelper2.f_InitClient(TestHelper);
			
			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);
				
			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, NContainer::CSecureByteVector{});
			DatabaseActor(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(60.0);
			auto Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
			DMibExpect(Database.m_Clients.f_GetLen(), ==, 0);
			
			f_TestCloudKeyManager(DatabaseActor, TestHelper, TestHelper2);
			
			auto HostID1 = TestHelper.f_GetClientHostID();
			auto HostID2 = TestHelper2.f_GetClientHostID();
			
			Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
			DMibExpect(Database.m_Clients.f_GetLen(), ==, 2);
			DMibExpect(Database.m_Clients[HostID1].m_Keys.f_GetLen(), ==, 2);
			DMibExpect(Database.m_Clients[HostID2].m_Keys.f_GetLen(), ==, 2);
			{
				DMibTestPath("ExistingKeys");
				
				auto DatabaseActor2
					= fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, NContainer::CSecureByteVector{})
				;
				DatabaseActor2(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(60.0);
				auto Database2 = DatabaseActor2(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
				
				auto fTestDatabase = [&]
					{
						DMibExpect(Database2.m_Clients[HostID1].m_Keys.f_GetLen(), ==, 2);
						DMibExpect(Database2.m_Clients[HostID2].m_Keys.f_GetLen(), ==, 2);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey0"], ==, Database2.m_Clients[HostID1].m_Keys["TestKey0"]);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey1"], ==, Database2.m_Clients[HostID1].m_Keys["TestKey1"]);
						DMibExpect(Database.m_Clients[HostID2].m_Keys["TestKey0"], ==, Database2.m_Clients[HostID2].m_Keys["TestKey0"]);
						DMibExpect(Database.m_Clients[HostID2].m_Keys["TestKey1"], ==, Database2.m_Clients[HostID2].m_Keys["TestKey1"]);
					}
				;
				
				
				{
					DMibTestPath("Load without running");
					fTestDatabase();
				}
				
				f_TestCloudKeyManager(DatabaseActor2, TestHelper, TestHelper2);
				
				{
					DMibTestPath("After running");
					Database2 = DatabaseActor2(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
					fTestDatabase();
				}
				
				{
					DMibTestPath("Check incorrect password is caught");
					auto DatabaseActor3
						= fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, "WrongPassword", NContainer::CSecureByteVector{})
					;
					
					DMibTest
						(
							DMibExpr(NMib::NTest::TCThrowsException<NException::CException>())
							== DMibLExpr(DatabaseActor3(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(60.0))
						)
					;
				}
			}
		};
		DMibTestSuite("PreCreatedKeys")
		{
			uint16 Port = 31401;
			CStrSecure Password = "Password";
			CStr DatabasePath = CFile::fs_GetProgramDirectory() + "/EncryptedFile";
			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);
			
			CDistributedActorTestHelperCombined TestHelper{Port};
			TestHelper.f_Init();
			
			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("PreCreatedKeysThread"), DatabasePath, Password, NContainer::CSecureByteVector{});
			
			CKeyManagerServerConfig Config;
			Config.m_DatabaseActor = DatabaseActor;
			Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(60.0);
		
			TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>(Config, fg_GetDistributionManager());
			KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_CallSync(60.0);
			
			CSymmetricKey AvailableKey;
			CSymmetricKey NextKey;
			{
				DMibTestPath("After initial keys pre created");
				auto Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				AvailableKey = Database.m_AvailableKeys[32u].f_GetFirst();
				NextKey = Database.m_AvailableKeys[32u].f_GetLast();
			}
			
			CStr Subscription = TestHelper.f_Subscribe("com.malterlib/Cloud/KeyManager");
			TCDistributedActor<CKeyManager> KeyManager = TestHelper.f_GetRemoteActor<CKeyManager>(Subscription);
		
			CSymmetricKey FirstKey = KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_CallSync(60.0);
			DMibExpect(FirstKey, ==, NextKey);
			
			{
				DMibTestPath("After first key popped");
				auto Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetFirst(), ==, AvailableKey);
			}
			
			KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_CallSync(60.0);
			
			{
				DMibTestPath("After second round of keys generated");
				auto Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLast(), ==, AvailableKey);
			}
			
			{
				DMibTestPath("Request key not pre created");
				CSymmetricKey NotPreCreatedKey = KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 64).f_CallSync(60.0);
				CSymmetricKey NotPreCreatedKey2 = KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey2", 64).f_CallSync(60.0);
				DMibExpect(NotPreCreatedKey, !=, NotPreCreatedKey2);
				
				auto Database = DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLast(), ==, AvailableKey);
			}
		};
	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
