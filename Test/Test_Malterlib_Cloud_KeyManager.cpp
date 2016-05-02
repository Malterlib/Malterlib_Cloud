
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Network/SSL>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Concurrency/TestHelpers>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>

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
		
		TCContinuation<void> f_WriteDatabase(CDatabase const &_Database)
		{
			m_Database = _Database;
			return TCContinuation<void>::fs_Finished();
		}
		
		TCContinuation<CDatabase> f_ReadDatabase()
		{
			return TCContinuation<CDatabase>::fs_Finished(m_Database);
		}
	};
	
	void f_TestCloudKeyManager
		(
			NMib::NConcurrency::TCActor<ICKeyManagerServerDatabase> const &_Database
			, CDistributedActorTestHelper &_TestHelper
			, CDistributedActorTestHelper &_TestHelper2
		)
	{
		CKeyManagerServerConfig Config;
		Config.m_DatabaseActor = _Database;
		//Config.m_PublicKeysForAllKeyManagers = ; TODO
		
		TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>(Config);
		
		_TestHelper.f_Subscribe("MalterlibCloudKeyManager");
		TCDistributedActor<CKeyManager> KeyManager = _TestHelper.f_GetRemoteActor<CKeyManager>();
		
		CSymmetricKey Key0 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey0", 32).f_CallSync(60.0);
		CSymmetricKey Key1 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey1", 32).f_CallSync(60.0);
		auto HostID1 = _TestHelper.f_GetClientHostID();
		
		DMibExpect(Key0, !=, Key1);
		auto Database = Config.m_DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_CallSync(60.0);
		DMibExpect(Key0, ==, Database.m_Clients[HostID1].m_Keys["TestKey0"]);
		DMibExpect(Key1, ==, Database.m_Clients[HostID1].m_Keys["TestKey1"]);
		
		_TestHelper2.f_Subscribe("MalterlibCloudKeyManager");

		auto HostID2 = _TestHelper2.f_GetClientHostID();
		TCDistributedActor<CKeyManager> KeyManager2 = _TestHelper2.f_GetRemoteActor<CKeyManager>();

		CSymmetricKey SecondKey0 = DMibCallActor(KeyManager2, CKeyManager::f_RequestKey, "TestKey0", 32).f_CallSync(60.0);
		CSymmetricKey SecondKey1 = DMibCallActor(KeyManager2, CKeyManager::f_RequestKey, "TestKey1", 32).f_CallSync(60.0);
		
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
			CDistributedActorTestHelper TestHelper;
			TestHelper.f_Init();
			
			CDistributedActorTestHelper TestHelper2;
			TestHelper2.f_InitClient(TestHelper);
			
			f_TestCloudKeyManager(fg_ConstructActor<CKeyManagerServerDatabaseImpl>(), TestHelper, TestHelper2);
		};
		DMibTestSuite("EncryptedFile")
		{
			CStr DatabasePath = CFile::fs_GetProgramDirectory() + "/EncryptedFile";
			CStrSecure Password = "Password";
			
			CDistributedActorTestHelper TestHelper;
			TestHelper.f_Init();
			
			CDistributedActorTestHelper TestHelper2;
			TestHelper2.f_InitClient(TestHelper);
			
			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);
				
			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, nullptr);
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
				
				auto DatabaseActor2 = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, nullptr);
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
			}
		};
	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
