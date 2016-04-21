
#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Concurrency/TestHelpers>

using namespace NMib::NConcurrency;
using namespace NMib::NCloud;

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
	
	void f_DoTests()
	{
		DMibTestSuite("General")
		{
			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabaseImpl>();
			CKeyManagerServerConfig Config;
			Config.m_DatabaseActor = DatabaseActor;
			
			auto &DatabaseImpl = DatabaseActor->f_AccessInternal();
			//Config.m_PublicKeysForAllKeyManagers = ; TODO
			
			TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>(Config);
			
			CDistributedActorTestHelper TestHelper;
			TestHelper.f_Init();
			TestHelper.f_Subscribe("MalterlibCloudKeyManager");
			auto &ClientCryptograhy = TestHelper.f_GetClientCryptograhySettings();
			TCDistributedActor<CKeyManager> KeyManager = TestHelper.f_GetRemoteActor<CKeyManager>();
			
			CSymmetricKey Key0 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey0").f_CallSync(60.0);
			CSymmetricKey Key1 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey1").f_CallSync(60.0);
			
			DMibExpect(Key0, !=, Key1);
			DMibExpect(Key0, ==, DatabaseImpl.m_Database.m_Clients[ClientCryptograhy.f_GetHostID()].m_Keys["TestKey0"]);
			DMibExpect(Key1, ==, DatabaseImpl.m_Database.m_Clients[ClientCryptograhy.f_GetHostID()].m_Keys["TestKey1"]);
			
			CDistributedActorTestHelper TestHelper2;
			TestHelper2.f_InitClient(TestHelper);
			TestHelper2.f_Subscribe("MalterlibCloudKeyManager");
			auto &ClientCryptograhy2 = TestHelper2.f_GetClientCryptograhySettings();
			TCDistributedActor<CKeyManager> KeyManager2 = TestHelper.f_GetRemoteActor<CKeyManager>();

			CSymmetricKey SecondKey0 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey0").f_CallSync(60.0);
			CSymmetricKey SecondKey1 = DMibCallActor(KeyManager, CKeyManager::f_RequestKey, "TestKey1").f_CallSync(60.0);
			
			DMibExpect(SecondKey0, !=, SecondKey1); 
			DMibExpect(SecondKey0, !=, Key0); 
			DMibExpect(SecondKey1, !=, Key1); 
			DMibExpect(SecondKey0, ==, DatabaseImpl.m_Database.m_Clients[ClientCryptograhy2.f_GetHostID()].m_Keys["TestKey0"]);
			DMibExpect(SecondKey1, ==, DatabaseImpl.m_Database.m_Clients[ClientCryptograhy2.f_GetHostID()].m_Keys["TestKey1"]);
		};		
	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
