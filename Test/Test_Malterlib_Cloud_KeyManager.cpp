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
					, EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions
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

		TCFuture<void> f_ConnectServers(TCSharedPointer<CTestState> _pClient, bool _bAddPermissions)
		{
			co_await f_ConnectTrustManagers(m_ServerTrustManager, _pClient->m_ServerTrustManager);

			CStr ClientHostID = co_await _pClient->m_ServerTrustManager(&CDistributedActorTrustManager::f_GetHostID).f_Timeout(g_Timeout, "Timeout");

			if (!_bAddPermissions)
				co_return {};

			co_await m_ServerTrustManager.f_CallActor(&CDistributedActorTrustManager::f_AddPermissions)
				(
					CPermissionIdentifiers(ClientHostID, "")
					, TCMap<CStr, CPermissionRequirements>{{"KeyManager/ServerSync", {}}}
					, EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions
				)
			;

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

		co_await KeyManagerServer(&CKeyManagerServer::f_Init, g_Timeout / 2);

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
		DMibExpect(Key0, ==, Database.m_Clients[HostID0].m_Keys["TestKey0"].m_Key);
		DMibExpect(Key1, ==, Database.m_Clients[HostID0].m_Keys["TestKey1"].m_Key);

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
		DMibExpect(SecondKey0, ==, Database.m_Clients[HostID1].m_Keys["TestKey0"].m_Key);
		DMibExpect(SecondKey1, ==, Database.m_Clients[HostID1].m_Keys["TestKey1"].m_Key);

		co_return {};
	}

	enum class EServerSyncTestFlag : uint32
	{
		mc_None = 0
		, mc_InvertInitOrder = DMibBit(0)
		, mc_InitAfterCreate = DMibBit(1)
		, mc_PreCreateKeysAfter = DMibBit(2)
		, mc_SimultaneousCreate = DMibBit(3)
		, mc_NoPermissions = DMibBit(4)
	};

	mint fs_CountAvailableKeys(auto &_Container)
	{
		mint nAvailaible = 0;

		for (auto &Keys : _Container)
			nAvailaible += Keys.f_GetLen();

		return nAvailaible;
	}

	TCFuture<void> f_TestServerSync(EServerSyncTestFlag _Test)
	{
		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
		CStr RootDirectoryServer0 = ProgramDirectory / "KeyManager/ServerSync/0";
		CStr RootDirectoryServer1 = ProgramDirectory / "KeyManager/ServerSync/1";

		TCSharedPointer<CTestState> pTestState0 = fg_Construct(RootDirectoryServer0);
		auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState0);
		co_await pTestState0->f_Init();

		TCSharedPointer<CTestState> pTestState1 = fg_Construct(RootDirectoryServer1);
		auto AsyncDestroy1 = co_await fg_AsyncDestroy(pTestState1);
		co_await pTestState1->f_Init();

		auto &ServerTrustManager0 = pTestState0->m_ServerTrustManager;
		auto &ServerTrustManager1 = pTestState1->m_ServerTrustManager;
		auto &Helper0TrustManager = pTestState0->m_Helper0TrustManager;
		auto &Helper1TrustManager = pTestState1->m_Helper0TrustManager;

		co_await pTestState0->f_ConnectServers(pTestState1, !fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions));
		co_await pTestState1->f_ConnectServers(pTestState0, !fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions));

		TCActor<CKeyManagerServer> KeyManagerServer0;
		NConcurrency::TCActor<ICKeyManagerServerDatabase> DatabaseActor0 = fg_ConstructActor<CKeyManagerServerDatabaseImpl>();
		{
			co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
			KeyManagerServer0 = fg_ConstructActor<CKeyManagerServer>
				(
					CKeyManagerServerConfig
					{
						.m_DatabaseActor = DatabaseActor0
						, .m_TrustManager = ServerTrustManager0
					}
				)
			;
		}
		auto AsyncDestroyServer0 = co_await fg_AsyncDestroy(KeyManagerServer0);

		TCActor<CKeyManagerServer> KeyManagerServer1;
		NConcurrency::TCActor<ICKeyManagerServerDatabase> DatabaseActor1 = fg_ConstructActor<CKeyManagerServerDatabaseImpl>();
		{
			co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
			KeyManagerServer1 = fg_ConstructActor<CKeyManagerServer>
				(
					CKeyManagerServerConfig
					{
						.m_DatabaseActor = DatabaseActor1
						, .m_TrustManager = ServerTrustManager1
					}
				)
			;
		}
		auto AsyncDestroyServer1 = co_await fg_AsyncDestroy(KeyManagerServer1);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_InvertInitOrder))
		{
			co_await KeyManagerServer1(&CKeyManagerServer::f_Init, g_Timeout / 2);
			co_await KeyManagerServer0(&CKeyManagerServer::f_Init, g_Timeout / 2);
		}
		else if (fg_IsSet(_Test, EServerSyncTestFlag::mc_InitAfterCreate))
			co_await KeyManagerServer0(&CKeyManagerServer::f_Init, g_Timeout / 2);
		else
		{
			co_await KeyManagerServer0(&CKeyManagerServer::f_Init, g_Timeout / 2);
			co_await KeyManagerServer1(&CKeyManagerServer::f_Init, g_Timeout / 2);
		}

		DMibTestMark;
		CTrustedSubscriptionTestHelper TestHelper0(Helper0TrustManager);
		TCDistributedActor<CKeyManager> KeyManager0 = co_await TestHelper0.f_SubscribeAsync<CKeyManager>();
		DMibTestMark;

		if (!fg_IsSet(_Test, EServerSyncTestFlag::mc_PreCreateKeysAfter))
			co_await KeyManagerServer0(&CKeyManagerServer::f_PreCreateKeys, 32, 4).f_Timeout(g_Timeout, "Timeout");

		CSymmetricKey Key0 = co_await KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;
		CSymmetricKey Key1 = co_await KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;

		DMibExpect(Key0, !=, Key1);

		auto HostID0 = co_await Helper0TrustManager(&CDistributedActorTrustManager::f_GetHostID);
		DMibTestMark;
		auto HostID1 = co_await Helper1TrustManager(&CDistributedActorTrustManager::f_GetHostID);
		DMibExpect(HostID0, !=, HostID1);

		auto ServerHostID0 = co_await ServerTrustManager0(&CDistributedActorTrustManager::f_GetHostID);
		DMibTestMark;
		auto ServerHostID1 = co_await ServerTrustManager1(&CDistributedActorTrustManager::f_GetHostID);
		DMibExpect(ServerHostID0, !=, ServerHostID1);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_PreCreateKeysAfter))
			co_await KeyManagerServer0(&CKeyManagerServer::f_PreCreateKeys, 32, 4).f_Timeout(g_Timeout, "Timeout");

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_InitAfterCreate))
			co_await KeyManagerServer1(&CKeyManagerServer::f_Init, g_Timeout / 2);

		auto Database0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		auto Database1 = co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
			DMibExpect(Database0.m_Clients, !=, Database1.m_Clients);
		else
			DMibExpect(Database0.m_Clients, ==, Database1.m_Clients);

		mint ExpectedAvailableKeys = 2;
		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_PreCreateKeysAfter))
			ExpectedAvailableKeys = 4;

		DMibExpect(fs_CountAvailableKeys(Database0.m_AvailableKeys), ==, ExpectedAvailableKeys);
		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
			DMibExpect(fs_CountAvailableKeys(Database1.m_AvailableKeys), ==, 0);
		else
			DMibExpect(fs_CountAvailableKeys(Database1.m_AvailableKeys), ==, ExpectedAvailableKeys);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
			DMibExpect(Database0.m_AvailableKeys, !=, Database1.m_AvailableKeys);
		else
			DMibExpect(Database0.m_AvailableKeys, ==, Database1.m_AvailableKeys);

		DMibTestMark;
		CTrustedSubscriptionTestHelper TestHelper1(Helper1TrustManager);
		TCDistributedActor<CKeyManager> KeyManager1 = co_await TestHelper1.f_SubscribeAsync<CKeyManager>();
		DMibTestMark;

		if (!fg_IsSet(_Test, EServerSyncTestFlag::mc_PreCreateKeysAfter))
			co_await KeyManagerServer1(&CKeyManagerServer::f_PreCreateKeys, 32, 4).f_Timeout(g_Timeout, "Timeout");

		CSymmetricKey SecondKey0 = co_await KeyManager1.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;
		CSymmetricKey SecondKey1 = co_await KeyManager1.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_Timeout(g_Timeout, "Timeout");
		DMibTestMark;

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_PreCreateKeysAfter))
			co_await KeyManagerServer1(&CKeyManagerServer::f_PreCreateKeys, 32, 4).f_Timeout(g_Timeout, "Timeout");

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_SimultaneousCreate))
		{
			auto Key0Future = KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey2", 32).f_Timeout(g_Timeout, "Timeout");
			auto Key1Future = KeyManager1.f_CallActor(&CKeyManager::f_RequestKey)("TestKey2", 32).f_Timeout(g_Timeout, "Timeout");
			CSymmetricKey ThirdKey0 = co_await fg_Move(Key0Future);
			DMibTestMark;
			CSymmetricKey ThirdKey1 = co_await fg_Move(Key1Future);
			DMibTestMark;
			DMibExpect(ThirdKey0, !=, ThirdKey1)(ETestFlag_NoValues);
			ExpectedAvailableKeys -= 2;
		}

		DMibExpect(SecondKey0, !=, SecondKey1)(ETestFlag_NoValues);
		DMibExpect(SecondKey0, !=, Key0)(ETestFlag_NoValues);
		DMibExpect(SecondKey1, !=, Key1)(ETestFlag_NoValues);

		auto DatabaseAfter0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		DMibExpect(Key0, ==, DatabaseAfter0.m_Clients[HostID0].m_Keys["TestKey0"].m_Key);
		DMibExpect(Key1, ==, DatabaseAfter0.m_Clients[HostID0].m_Keys["TestKey1"].m_Key);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
		{
			DMibExpectTrue(DatabaseAfter0.m_Clients[HostID1].m_Keys["TestKey0"].m_Key.f_IsEmpty());
			DMibExpectTrue(DatabaseAfter0.m_Clients[HostID1].m_Keys["TestKey1"].m_Key.f_IsEmpty());
		}
		else
		{
			DMibExpect(SecondKey0, ==, DatabaseAfter0.m_Clients[HostID1].m_Keys["TestKey0"].m_Key);
			DMibExpect(SecondKey1, ==, DatabaseAfter0.m_Clients[HostID1].m_Keys["TestKey1"].m_Key);
		}

		auto DatabaseAfter1 = co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
		{
			DMibExpectTrue(DatabaseAfter1.m_Clients[HostID0].m_Keys["TestKey0"].m_Key.f_IsEmpty());
			DMibExpectTrue(DatabaseAfter1.m_Clients[HostID0].m_Keys["TestKey1"].m_Key.f_IsEmpty());
		}
		else
		{
			DMibExpect(Key0, ==, DatabaseAfter1.m_Clients[HostID0].m_Keys["TestKey0"].m_Key);
			DMibExpect(Key1, ==, DatabaseAfter1.m_Clients[HostID0].m_Keys["TestKey1"].m_Key);
		}
		DMibExpect(SecondKey0, ==, DatabaseAfter1.m_Clients[HostID1].m_Keys["TestKey0"].m_Key);
		DMibExpect(SecondKey1, ==, DatabaseAfter1.m_Clients[HostID1].m_Keys["TestKey1"].m_Key);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
		{
			DMibExpect(DatabaseAfter0.m_Clients, !=, DatabaseAfter1.m_Clients);
			DMibExpect(DatabaseAfter0.m_AvailableKeys, !=, DatabaseAfter1.m_AvailableKeys);
		}
		else
		{
			DMibExpect(DatabaseAfter0.m_Clients, ==, DatabaseAfter1.m_Clients);
			DMibExpect(DatabaseAfter0.m_AvailableKeys, ==, DatabaseAfter1.m_AvailableKeys);
		}

		DMibExpect(fs_CountAvailableKeys(DatabaseAfter0.m_AvailableKeys), ==, ExpectedAvailableKeys);
		DMibExpect(fs_CountAvailableKeys(DatabaseAfter1.m_AvailableKeys), ==, ExpectedAvailableKeys);

		if (fg_IsSet(_Test, EServerSyncTestFlag::mc_NoPermissions))
			co_return {};

		auto Result0 = co_await KeyManagerServer0(&CKeyManagerServer::f_RemoveVerifiedHosts, TCSet<CStr>{ServerHostID0}).f_Timeout(g_Timeout, "Timeout").f_Wrap();
		DMibExpectException(Result0.f_Get(), DMibErrorInstance("The host ID {} you are trying to remove is still running"_f << ServerHostID0));

		auto Result1 = co_await KeyManagerServer1(&CKeyManagerServer::f_RemoveVerifiedHosts, TCSet<CStr>{ServerHostID0}).f_Timeout(g_Timeout, "Timeout").f_Wrap();
		DMibExpectException(Result1.f_Get(), DMibErrorInstance("The host ID {} you are trying to remove is still running"_f << ServerHostID0));

		auto DatabaseAfterRemove0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
		auto DatabaseAfterRemove1 = co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

		DMibExpect(DatabaseAfterRemove0.m_Clients[HostID0].m_Keys["TestKey0"].m_VerifiedOnServers, ==, (TCSet<CStr>{ServerHostID0, ServerHostID1}));

		DMibExpect(DatabaseAfterRemove0.m_Clients, ==, DatabaseAfter0.m_Clients);
		DMibExpect(DatabaseAfterRemove0.m_AvailableKeys, ==, DatabaseAfter0.m_AvailableKeys);
		DMibExpect(DatabaseAfterRemove1.m_Clients, ==, DatabaseAfter0.m_Clients);
		DMibExpect(DatabaseAfterRemove1.m_AvailableKeys, ==, DatabaseAfter0.m_AvailableKeys);

		co_await fg_Move(KeyManagerServer1).f_Destroy();

		co_await KeyManagerServer0(&CKeyManagerServer::f_RemoveVerifiedHosts, TCSet<CStr>{ServerHostID1}).f_Timeout(g_Timeout, "Timeout");

		auto DatabaseAfterRemoveSuccess0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

		DMibExpect(DatabaseAfterRemoveSuccess0.m_Clients[HostID0].m_Keys["TestKey0"].m_VerifiedOnServers, ==, TCSet<CStr>{ServerHostID0});

		co_return {};
	}

	TCFuture<void> f_TestServerSyncMinServers()
	{
		CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
		CStr RootDirectoryServer0 = ProgramDirectory / "KeyManager/ServerSyncMinServers/0";
		CStr RootDirectoryServer1 = ProgramDirectory / "KeyManager/ServerSyncMinServers/1";

		TCSharedPointer<CTestState> pTestState0 = fg_Construct(RootDirectoryServer0);
		auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState0);
		co_await pTestState0->f_Init();

		TCSharedPointer<CTestState> pTestState1 = fg_Construct(RootDirectoryServer1);
		auto AsyncDestroy1 = co_await fg_AsyncDestroy(pTestState1);
		co_await pTestState1->f_Init();

		auto &ServerTrustManager0 = pTestState0->m_ServerTrustManager;
		auto &ServerTrustManager1 = pTestState1->m_ServerTrustManager;
		auto &Helper0TrustManager = pTestState0->m_Helper0TrustManager;

		co_await pTestState0->f_ConnectServers(pTestState1, true);
		co_await pTestState1->f_ConnectServers(pTestState0, true);

		TCActor<CKeyManagerServer> KeyManagerServer0;
		NConcurrency::TCActor<ICKeyManagerServerDatabase> DatabaseActor0 = fg_ConstructActor<CKeyManagerServerDatabaseImpl>();
		{
			co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
			KeyManagerServer0 = fg_ConstructActor<CKeyManagerServer>
				(
					CKeyManagerServerConfig
					{
						.m_DatabaseActor = DatabaseActor0
						, .m_TrustManager = ServerTrustManager0
						, .m_CreateNewKeyMinServers = 2
					}
				)
			;
		}
		auto AsyncDestroyServer0 = co_await fg_AsyncDestroy(KeyManagerServer0);

		TCActor<CKeyManagerServer> KeyManagerServer1;
		NConcurrency::TCActor<ICKeyManagerServerDatabase> DatabaseActor1 = fg_ConstructActor<CKeyManagerServerDatabaseImpl>();
		{
			co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_Initialize).f_Timeout(g_Timeout, "Timeout");
			KeyManagerServer1 = fg_ConstructActor<CKeyManagerServer>
				(
					CKeyManagerServerConfig
					{
						.m_DatabaseActor = DatabaseActor1
						, .m_TrustManager = ServerTrustManager1
						, .m_CreateNewKeyMinServers = 2
					}
				)
			;
		}
		auto AsyncDestroyServer1 = co_await fg_AsyncDestroy(KeyManagerServer1);

		co_await KeyManagerServer0(&CKeyManagerServer::f_Init, g_Timeout / 2);

		DMibTestMark;
		CTrustedSubscriptionTestHelper TestHelper0(Helper0TrustManager);
		TCDistributedActor<CKeyManager> KeyManager0 = co_await TestHelper0.f_SubscribeAsync<CKeyManager>();
		DMibTestMark;

		co_await KeyManagerServer0(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_Timeout(g_Timeout, "Timeout");

		{
			DMibTestPath("Use available key");

			DMibExpectException
				(
					KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_CallSync(g_Timeout)
					, DMibErrorInstance("Only 1 of 2 key managers available to verify available key use")
				)
			;
			auto Database0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

			DMibExpect(fs_CountAvailableKeys(Database0.m_AvailableKeys), ==, 2);
		}

		CSymmetricKey DirectCreateKey;
		{
			DMibTestPath("Direct create");

			DMibExpectException
				(
					KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 16).f_CallSync(g_Timeout)
					, DMibErrorInstance("Key has only been verified on 1 of 2 required servers")
				)
			;

			auto Database0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
			
			DMibExpect(fs_CountAvailableKeys(Database0.m_AvailableKeys), ==, 2);
			DMibAssert(Database0.m_Clients.f_GetLen(), ==, 1);
			DMibAssert(Database0.m_Clients.f_FindAny()->m_Keys.f_GetLen(), ==, 1);
			DMibExpect(Database0.m_Clients.f_FindAny()->m_Keys.f_FindAny()->m_Key.f_GetLen(), ==, 16);
			DMibExpect(Database0.m_Clients.f_FindAny()->m_Keys.f_FindAny()->m_VerifiedOnServers.f_GetLen(), ==, 1);

			DirectCreateKey = Database0.m_Clients.f_FindAny()->m_Keys.f_FindAny()->m_Key;
		}

		{
			DMibTestPath("Connect second server");

			co_await KeyManagerServer1(&CKeyManagerServer::f_Init, g_Timeout / 2);
			auto Database0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
			auto Database1 = co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

			DMibExpect(Database0.m_Clients, ==, Database1.m_Clients);
			DMibExpect(Database0.m_AvailableKeys, ==, Database1.m_AvailableKeys);

			DMibAssert(Database0.m_Clients.f_GetLen(), ==, 1);
			DMibAssert(Database0.m_Clients.f_FindAny()->m_Keys.f_GetLen(), ==, 1);
			DMibExpect(Database0.m_Clients.f_FindAny()->m_Keys.f_FindAny()->m_Key.f_GetLen(), ==, 16);
			DMibExpect(Database0.m_Clients.f_FindAny()->m_Keys.f_FindAny()->m_VerifiedOnServers.f_GetLen(), ==, 2);
		}

		{
			DMibTestPath("Direct retrieve after connect");

			auto Key0 = co_await KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 16).f_Timeout(g_Timeout, "Timeout");
			DMibExpect(Key0, ==, DirectCreateKey);
		}

		{
			DMibTestPath("Use available key after connect");

			auto Key1 = co_await KeyManager0.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 32).f_Timeout(g_Timeout, "Timeout");

			DMibExpect(Key1.f_GetLen(), ==, 32);

			auto Database0 = co_await DatabaseActor0(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
			auto Database1 = co_await DatabaseActor1(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");

			DMibExpect(fs_CountAvailableKeys(Database0.m_AvailableKeys), ==, 1);

			DMibExpect(Database0.m_Clients, ==, Database1.m_Clients);
			DMibExpect(Database0.m_AvailableKeys, ==, Database1.m_AvailableKeys);
		}

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
						DMibExpect(Database.m_Clients[HostID0].m_Keys["TestKey0"].m_Key, ==, Database2.m_Clients[HostID0].m_Keys["TestKey0"].m_Key);
						DMibExpect(Database.m_Clients[HostID0].m_Keys["TestKey1"].m_Key, ==, Database2.m_Clients[HostID0].m_Keys["TestKey1"].m_Key);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey0"].m_Key, ==, Database2.m_Clients[HostID1].m_Keys["TestKey0"].m_Key);
						DMibExpect(Database.m_Clients[HostID1].m_Keys["TestKey1"].m_Key, ==, Database2.m_Clients[HostID1].m_Keys["TestKey1"].m_Key);
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
		DMibTestSuite("PreCreateKeys") -> TCFuture<void>
		{
			CStr ProgramDirectory = CFile::fs_GetProgramDirectory();
			CStr RootDirectory = ProgramDirectory / "KeyManager/PreCreateKeys";
			CStr DatabasePath = RootDirectory / "EncryptedFile.PreCreateKeys";

			CStrSecure Password = "Password";

			fg_TestAddCleanupPath(DatabasePath);

			if (CFile::fs_FileExists(DatabasePath))
				CFile::fs_DeleteFile(DatabasePath);

			TCSharedPointer<CTestState> pTestState = fg_Construct(RootDirectory);
			auto AsyncDestroy0 = co_await fg_AsyncDestroy(pTestState);
			co_await pTestState->f_Init();

			auto &ServerTrustManager = pTestState->m_ServerTrustManager;
			auto &Helper0TrustManager = pTestState->m_Helper0TrustManager;

			auto DatabaseActor = fg_ConstructActor<CKeyManagerServerDatabase_EncryptedFile>(fg_Construct("PreCreateKeysThread"), DatabasePath, Password, NContainer::CSecureByteVector{});

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

			co_await KeyManagerServer(&CKeyManagerServer::f_Init, g_Timeout / 2);
			co_await KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_Timeout(g_Timeout, "Timeout");

			CSymmetricKey AvailableKey;
			CSymmetricKey NextKey;
			{
				DMibTestPath("After initial keys pre created");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibAssert(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibAssert(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				AvailableKey = *Database.m_AvailableKeys[32u].f_FindLargest();
				NextKey = *Database.m_AvailableKeys[32u].f_FindSmallest();
			}

			CTrustedSubscriptionTestHelper TestHelper0(Helper0TrustManager);
			TCDistributedActor<CKeyManager> KeyManager = co_await TestHelper0.f_SubscribeAsync<CKeyManager>();

			CSymmetricKey FirstKey = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey0", 32).f_Timeout(g_Timeout, "Timeout");
			DMibExpect(FirstKey, ==, NextKey);

			{
				DMibTestPath("After first key popped");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibAssert(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibAssert(Database.m_AvailableKeys[32u].f_GetLen(), ==, 1);
				DMibExpect(*Database.m_AvailableKeys[32u].f_FindSmallest(), ==, AvailableKey);
			}

			co_await KeyManagerServer(&CKeyManagerServer::f_PreCreateKeys, 32, 2).f_Timeout(g_Timeout, "Timeout");

			{
				DMibTestPath("After second round of keys generated");
				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibAssert(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibAssert(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpectTrue(Database.m_AvailableKeys[32u].f_FindEqual(AvailableKey));
			}

			{
				DMibTestPath("Request key not pre created");
				CSymmetricKey NotPreCreateKey = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey1", 64).f_Timeout(g_Timeout, "Timeout");
				CSymmetricKey NotPreCreateKey2 = co_await KeyManager.f_CallActor(&CKeyManager::f_RequestKey)("TestKey2", 64).f_Timeout(g_Timeout, "Timeout");
				DMibExpect(NotPreCreateKey, !=, NotPreCreateKey2);

				auto Database = co_await DatabaseActor(&ICKeyManagerServerDatabase::f_ReadDatabase).f_Timeout(g_Timeout, "Timeout");
				DMibAssert(Database.m_AvailableKeys.f_GetLen(), ==, 1);
				DMibAssert(Database.m_AvailableKeys[32u].f_GetLen(), ==, 2);
				DMibExpectTrue(Database.m_AvailableKeys[32u].f_FindEqual(AvailableKey));
			}

			co_return {};
		};
		DMibTestSuite("ServerSync") -> TCFuture<void>
		{
			for (mint i = 0; i < 1; ++i)
			{
				DMibTestPath(i == 0 ? "PreCreate Before" : "PreCreate After");

				auto Flags = i == 0 ? EServerSyncTestFlag::mc_None : EServerSyncTestFlag::mc_PreCreateKeysAfter;

				{
					DMibTestPath("Normal");
					co_await f_TestServerSync(Flags);
				}
				{
					DMibTestPath("Invert Init Order");
					co_await f_TestServerSync(Flags | EServerSyncTestFlag::mc_InvertInitOrder);
				}
				{
					DMibTestPath("Init After Create");
					co_await f_TestServerSync(Flags | EServerSyncTestFlag::mc_InitAfterCreate);
				}
				{
					DMibTestPath("Simultaneous Create");
					co_await f_TestServerSync(Flags | EServerSyncTestFlag::mc_SimultaneousCreate);
				}
				{
					DMibTestPath("No Permission");
					co_await f_TestServerSync(Flags | EServerSyncTestFlag::mc_NoPermissions);
				}
			}
			co_return {};
		};
		DMibTestSuite("ServerSyncMinServers") -> TCFuture<void>
		{
			co_await f_TestServerSyncMinServers();
			co_return {};
		};

	}
};

DMibTestRegister(CKeyManager_Tests, Malterlib::Cloud);
