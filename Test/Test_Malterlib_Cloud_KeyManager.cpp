// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Test/Test>
#include <Mib/Cloud/KeyManager>
#include <Mib/Cloud/KeyManagerServer>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Cloud/KeyManagerDatabases/EncryptedFile>
#include <Mib/Test/Exception>
#include <Mib/Container/Vector>

using namespace NMib;
using namespace NMib::NConcurrency;
using namespace NMib::NCloud;
using namespace NMib::NFile;
using namespace NMib::NNetwork;
using namespace NMib::NContainer;
using namespace NMib::NStorage;
using namespace NMib::NStr;
using namespace NMib::NTest;

static fp64 g_Timeout = 60.0 * NMib::NTest::gc_TimeoutMultiplier;

class CKeyManager_Tests : public NMib::NTest::CTest
{
public:

	struct CKeyManagerServerDatabaseImpl : public ICKeyManagerServerDatabase
	{
		static constexpr bool mc_bAllowInternalAccess = true;

		CDatabase m_Database;

		TCFuture<void> f_Initialize()
		{
			co_return {};
		}

		TCFuture<void> f_WriteDatabase(CDatabase const &_Database)
		{
			m_Database = _Database;
			co_return {};
		}

		TCFuture<CDatabase> f_ReadDatabase()
		{
			co_return m_Database;
		}
	};

	struct CTestState : public CAllowUnsafeThis
	{
		CTestState(CTestState const &) = delete;
		CTestState(CTestState &&) = delete;

		CTestState(CStr const &_RootDirectory)
			: m_RootDirectory(_RootDirectory)
		{
		}

		~CTestState()
		{
		}

		TCFuture<void> f_Destroy()
		{
			if (m_Helper0TrustManager)
				co_await fg_Move(m_Helper0TrustManager).f_Destroy();
			if (m_Helper1TrustManager)
				co_await fg_Move(m_Helper1TrustManager).f_Destroy();
			if (m_ServerTrustManager)
				co_await fg_Move(m_ServerTrustManager).f_Destroy();

			if (m_Helper0TrustManagerState.m_Database)
				co_await fg_Move(m_Helper0TrustManagerState.m_Database).f_Destroy();

			if (m_Helper1TrustManagerState.m_Database)
				co_await fg_Move(m_Helper1TrustManagerState.m_Database).f_Destroy();

			if (m_ServerTrustManagerState.m_Database)
				co_await fg_Move(m_ServerTrustManagerState.m_Database).f_Destroy();

			co_return {};
		}

		TCFuture<void> f_ConnectTrustManagers
			(
				TCActor<CDistributedActorTrustManager> _ServerTrustManager
				, TCActor<CDistributedActorTrustManager> _ClientTrustManager
			)
		{
			CStr ServerHostID = co_await _ServerTrustManager(&CDistributedActorTrustManager::f_GetHostID).f_Timeout(g_Timeout, "Timeout");

			auto Ticket = co_await _ServerTrustManager(&CDistributedActorTrustManager::f_GenerateConnectionTicket, m_ServerAddress, nullptr, nullptr).f_Timeout(g_Timeout, "Timeout");
			auto HostInfo = co_await _ClientTrustManager(&CDistributedActorTrustManager::f_AddClientConnection, fg_Move(Ticket.m_Ticket), g_Timeout, 1).f_Timeout(g_Timeout, "Timeout");

			co_await _ClientTrustManager
				(
					&CDistributedActorTrustManager::f_AllowHostsForNamespace
					, CKeyManager::mc_pDefaultNamespace
					, TCSet<CStr>{ServerHostID}
					, EDistributedActorTrustManagerOrderingFlag_None
				)
			;

			co_return {};
		}

		TCFuture<void> f_Init()
		{
			m_ServerTrustManager = m_ServerTrustManagerState.f_TrustManager("KeyManagerServer");
			m_Helper0TrustManager = m_Helper0TrustManagerState.f_TrustManager("TestHelper0");
			m_Helper1TrustManager = m_Helper1TrustManagerState.f_TrustManager("TestHelper1");

			m_ServerAddress.m_URL = "wss://[UNIX(666):{}]/"_f << fg_GetSafeUnixSocketPath("{}/server.sock"_f << m_RootDirectory);
			co_await m_ServerTrustManager(&CDistributedActorTrustManager::f_AddListen, m_ServerAddress).f_Timeout(g_Timeout, "Timeout");

			co_await f_ConnectTrustManagers(m_ServerTrustManager, m_Helper0TrustManager);
			co_await f_ConnectTrustManagers(m_ServerTrustManager, m_Helper1TrustManager);

			co_return {};
		}

		CDistributedActorTrustManager_Address m_ServerAddress;

		CTrustManagerTestHelper m_ServerTrustManagerState;
		TCActor<CDistributedActorTrustManager> m_ServerTrustManager;

		CTrustManagerTestHelper m_Helper0TrustManagerState;
		TCActor<CDistributedActorTrustManager> m_Helper0TrustManager;

		CTrustManagerTestHelper m_Helper1TrustManagerState;
		TCActor<CDistributedActorTrustManager> m_Helper1TrustManager;

		CStr m_RootDirectory;
	};

	TCFuture<void> f_TestCloudKeyManager(TCActor<ICKeyManagerServerDatabase> _Database, TCSharedPointer<CTestState> _pTestState)
	{
		auto &ServerTrustManager = _pTestState->m_ServerTrustManager;
		auto &Helper0TrustManager = _pTestState->m_Helper0TrustManager;
		auto &Helper1TrustManager = _pTestState->m_Helper1TrustManager;

		auto HostID0 = co_await Helper0TrustManager(&CDistributedActorTrustManager::f_GetHostID);
		auto HostID1 = co_await Helper1TrustManager(&CDistributedActorTrustManager::f_GetHostID);

		co_await _Database(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");

		DMibTestMark;

		TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>
			(
				CKeyManagerServerConfig
				{
					.m_DatabaseActor = _Database
					, .m_TrustManager = ServerTrustManager
				}
			)
		;
		auto AsyncDestroyServer = co_await fg_AsyncDestroy(KeyManagerServer);

		co_await KeyManagerServer(&CKeyManagerServer::f_Init);

		CTrustedSubscriptionTestHelper TestHelper0(Helper0TrustManager);
		DMibTestMark;
		TCDistributedActor<CKeyManager> KeyManager = co_await TestHelper0.f_SubscribeAsync<CKeyManager>();
		DMibTestMark;

		CSymmetricKey Key0 = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;
		CSymmetricKey Key1 = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;

		DMibExpect(Key0, !=, Key1);
		auto Database = co_await _Database(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		DMibExpect(Key0, ==, Database.m_Clients[HostID0].m_Keys["TestKey0"]);
		DMibExpect(Key1, ==, Database.m_Clients[HostID0].m_Keys["TestKey1"]);

		CTrustedSubscriptionTestHelper TestHelper1(Helper1TrustManager);
		DMibTestMark;
		TCDistributedActor<CKeyManager> KeyManager2 = co_await TestHelper1.f_SubscribeAsync<CKeyManager>();
		DMibTestMark;

		CSymmetricKey SecondKey0 = co_await KeyManager2.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;
		CSymmetricKey SecondKey1 = co_await KeyManager2.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_Timeout(g_Timeout, "Timeout");

		DMibExpect(SecondKey0, !=, SecondKey1);
		DMibExpect(SecondKey0, !=, Key0);
		DMibExpect(SecondKey1, !=, Key1);
		Database = co_await _Database(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		DMibExpect(SecondKey0, ==, Database.m_Clients[HostID1].m_Keys["TestKey0"]);
		DMibExpect(SecondKey1, ==, Database.m_Clients[HostID1].m_Keys["TestKey1"]);

		co_return {};
	}

	void f_DoTests()
	{
		DMibTestSuite("General") -> TCFuture<void>
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory / "KeyManager/General";

			TCSharedPointer<CTestState> pTestState = fg_Construct(RootDirectory);
			auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState);
			co_await pTestState->f_Init();

			co_await f_TestCloudKeyManager(fg_ConstructActor<CKeyManagerServerDatabaseImpl>(), pTestState);

			co_return {};
		};
		DMibTestSuite("EncryptedFile") -> TCFuture<void>
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory / "KeyManager/EncryptedFile";
			CStr DatabasePath = RootDirectory / "EncryptedFile.EncryptedFile";

			fg_TestAddCleanupPath(DatabasePath);

			TCSharedPointer<CTestState> pTestState = fg_Construct(RootDirectory);
			auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState);
			co_await pTestState->f_Init();
			
			CStrSecure Password = "Password";

			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);

			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, NContainer::CSecureByteVector{});
			co_await DatabaseActor(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
			auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
			DMibExpect(Database.m_Clients.f_GetLen(), ==, 0);

			co_await f_TestCloudKeyManager(DatabaseActor, pTestState);

			auto &Helper0TrustManager = pTestState->m_Helper0TrustManager;
			auto &Helper1TrustManager = pTestState->m_Helper1TrustManager;

			auto HostID0 = co_await Helper0TrustManager(&CDistributedActorTrustManager::f_GetHostID);
			auto HostID1 = co_await Helper1TrustManager(&CDistributedActorTrustManager::f_GetHostID);

			Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
			DMibExpect(Database.m_Clients.f_GetLen(), ==, 2);
			DMibExpect(Database.m_Clients[HostID0].m_Keys.f_GetLen(), ==, 2);
			DMibExpect(Database.m_Clients[HostID1].m_Keys.f_GetLen(), ==, 2);
			{
				DMibTestPath("ExistingKeys");

				auto DatabaseActor2
					= fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("EncryptedFileThread"), DatabasePath, Password, NContainer::CSecureByteVector{})
				;
				co_await DatabaseActor2(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
				auto Database2 = co_await DatabaseActor2(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

				auto fTestDatabase = [&]
					{
						DMibExpect(Database2.m_Clients[HostID0].m_Keys.f_GetLen(), ==, 2);
						DMibExpect(Database2.m_Clients[HostID1].m_Keys.f_GetLen(), ==, 2);
						DMibExpect(Database.m_Clients[HostID0].m_Keys["TestKey0"], ==, Database2.m_Clients[HostID0].m_Keys["TestKey0"]);
						DMibExpect(Database.m_Clients[HostID0].m_Keys["TestKey1"], ==, Database2.m_Clients[HostID0].m_Keys["TestKey1"]);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey0"], ==, Database2.m_Clients[HostID1].m_Keys["TestKey0"]);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey1"], ==, Database2.m_Clients[HostID1].m_Keys["TestKey1"]);
					}
				;


				{
					DMibTestPath("Load without running");
					fTestDatabase();
				}

				DMibTestMark;

				co_await f_TestCloudKeyManager(DatabaseActor2, pTestState);

				DMibTestMark;

				{
					DMibTestPath("After running");
					Database2 = co_await DatabaseActor2(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
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
							== DMibLExpr(DatabaseActor3(&ICKeyManagerServerDatabase::f_Initialize).f_CallSync(g_Timeout))
						)
					;
				}
				co_return {};
			}
		};
		DMibTestSuite("PreCreatedKeys") -> TCFuture<void>
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory / "KeyManager/PreCreatedKeys";
			CStr DatabasePath = RootDirectory / "EncryptedFile.PreCreatedKeys";

			CStrSecure Password = "Password";

			fg_TestAddCleanupPath(DatabasePath);

			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);

			TCSharedPointer<CTestState> pTestState = fg_Construct(RootDirectory);
			auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState);
			co_await pTestState->f_Init();

			auto &ServerTrustManager = pTestState->m_ServerTrustManager;
			auto &Helper0TrustManager = pTestState->m_Helper0TrustManager;

			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("PreCreatedKeysThread"), DatabasePath, Password, NContainer::CSecureByteVector{});

			co_await DatabaseActor(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");

			TCActor<CKeyManagerServer> KeyManagerServer = fg_ConstructActor<CKeyManagerServer>
				(
					CKeyManagerServerConfig
					{
						.m_DatabaseActor = DatabaseActor
						, .m_TrustManager = ServerTrustManager
					}
				)
			;
			auto AsyncDestroyServer = co_await fg_AsyncDestroy(KeyManagerServer);

			co_await KeyManagerServer(&CKeyManagerServer::f_Init);
			co_await KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_Timeout(g_Timeout, "Timeout");

			CSymmetricKey AvailableKey;
			CSymmetricKey NextKey;
			{
				DMibTestPath("After initial keys pre created");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				AvailableKey = Database.m_AvailableKeys[32u].f_GetFirst();
				NextKey = Database.m_AvailableKeys[32u].f_GetLast();
			}

			CTrustedSubscriptionTestHelper TestHelper0(Helper0TrustManager);
			TCDistributedActor<CKeyManager> KeyManager = co_await TestHelper0.f_SubscribeAsync<CKeyManager>();

			CSymmetricKey FirstKey = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
			DMibExpect(FirstKey, ==, NextKey);

			{
				DMibTestPath("After first key popped");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetFirst(), ==, AvailableKey);
			}

			co_await KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_Timeout(g_Timeout, "Timeout");

			{
				DMibTestPath("After second round of keys generated");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLast(), ==, AvailableKey);
			}

			{
				DMibTestPath("Request key not pre created");
				CSymmetricKey NotPreCreatedKey = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 64).f_Timeout(g_Timeout, "Timeout");
				CSymmetricKey NotPreCreatedKey2 = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey2", 64).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(NotPreCreatedKey, !=, NotPreCreatedKey2);

				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpect(Database.m_AvailableKeys[32u].f_GetLast(), ==, AvailableKey);
			}

			co_return {};
		};
	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
