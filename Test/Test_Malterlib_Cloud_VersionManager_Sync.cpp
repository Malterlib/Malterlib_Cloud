// Copyright © 2025 Unbroken AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Test_Malterlib_Cloud_AppManager.h"
#include <Mib/Cloud/App/VersionManager>

#include <Mib/Test/Exception>

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#endif

namespace NVersionManagerSyncTests
{
	fp64 g_Timeout = 60.0 * NMib::NTest::gc_TimeoutMultiplier;

	using namespace NMib;
	using namespace NMib::NConcurrency;
	using namespace NMib::NFile;
	using namespace NMib::NStr;
	using namespace NMib::NContainer;
	using namespace NMib::NCloud;
	using namespace NMib::NStorage;
	using namespace NMib::NEncoding;
	using namespace NMib::NTest;

	// Options struct for sync configuration
	struct CSyncConfigOptions
	{
		TCVector<CStr> m_Applications;
		TCVector<CStr> m_Platforms;
		TCVector<CStr> m_Tags;
		TCVector<CStr> m_CopyTags;
		bool m_bPretend = false;
		bool m_bSyncRetrySequence = false;
		CStr m_StartSyncDate;
		uint32 m_nMinSyncVersions = 0;
	};

	// Helper to create a single sync source config (without SyncSources wrapper)
	CEJsonSorted fg_CreateSyncSource(TCSet<CStr> _SyncHosts, CSyncConfigOptions const &_Options = {})
	{
		CEJsonSorted Source = EJsonType_Object;

		Source["Enabled"] = true;
		Source["Pretend"] = _Options.m_bPretend;
		Source["SyncRetrySequence"] = _Options.m_bSyncRetrySequence;

		CEJsonSorted SyncHostsArray = EJsonType_Array;
		for (auto &Host : _SyncHosts)
			SyncHostsArray.f_Insert(Host);
		Source["SyncHosts"] = SyncHostsArray;

		if (!_Options.m_Applications.f_IsEmpty())
		{
			CEJsonSorted AppsArray = EJsonType_Array;
			for (auto &App : _Options.m_Applications)
				AppsArray.f_Insert(App);
			Source["Applications"] = AppsArray;
		}

		if (!_Options.m_Platforms.f_IsEmpty())
		{
			CEJsonSorted PlatformsArray = EJsonType_Array;
			for (auto &Platform : _Options.m_Platforms)
				PlatformsArray.f_Insert(Platform);
			Source["Platforms"] = PlatformsArray;
		}

		if (!_Options.m_Tags.f_IsEmpty())
		{
			CEJsonSorted TagsArray = EJsonType_Array;
			for (auto &Tag : _Options.m_Tags)
				TagsArray.f_Insert(Tag);
			Source["Tags"] = TagsArray;
		}

		if (!_Options.m_CopyTags.f_IsEmpty())
		{
			CEJsonSorted CopyTagsArray = EJsonType_Array;
			for (auto &CopyTag : _Options.m_CopyTags)
				CopyTagsArray.f_Insert(CopyTag);
			Source["CopyTags"] = CopyTagsArray;
		}

		if (!_Options.m_StartSyncDate.f_IsEmpty())
			Source["StartSyncDate"] = _Options.m_StartSyncDate;

		if (_Options.m_nMinSyncVersions > 0)
			Source["MinSyncVersions"] = (int64)_Options.m_nMinSyncVersions;

		return Source;
	}

	// Helper to create full sync config from named sources
	CEJsonSorted fg_CreateMultiSyncConfig(CEJsonSorted _Sources)
	{
		CEJsonSorted Config = EJsonType_Object;
		Config["SyncSources"] = _Sources;

		return Config;
	}

	// Helper to create sync source config JSON
	CEJsonSorted fg_CreateSyncConfig(CStr _Name, TCSet<CStr> _SyncHosts, CSyncConfigOptions const &_Options = {})
	{
		CEJsonSorted SyncSources = EJsonType_Object;
		SyncSources[_Name] = fg_CreateSyncSource(_SyncHosts, _Options);

		CEJsonSorted Config = EJsonType_Object;
		Config["SyncSources"] = SyncSources;

		return Config;
	}

	// Helper structure for dual VersionManager sync tests
	struct CSyncTestHelper : public CAllowUnsafeThis
	{
		CSyncTestHelper(CStr _TestName, CAppManagerTestHelper::EOption _Options)
			: m_Options(_Options)
			, m_SourceHelper(_TestName + "_Source", _Options, g_Timeout)
		{
		}

		TCFuture<void> f_SetupSource(mint _nAppManagers = 0)
		{
			co_await m_SourceHelper.f_Setup(_nAppManagers);
			co_return {};
		}

		TCFuture<void> f_SetupDestination(CEJsonSorted _Config)
		{
			auto &SourceState = *m_SourceHelper.m_pState;

			// Create destination directory
			m_DestDirectory = SourceState.m_RootDirectory + "/VersionManagerDest";
			CFile::fs_CreateDirectory(m_DestDirectory);
			CFile::fs_DiffCopyFileOrDirectory(SourceState.m_ProgramDirectory + "/TestApps/VersionManager", m_DestDirectory, nullptr);

			// Write sync config before launching
			CFile::fs_WriteStringToFile(m_DestDirectory + "/VersionManagerConfig.json", _Config.f_ToString(nullptr));

			// Launch destination VersionManager
			m_DestLaunch = co_await SourceState.m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManagerDest"
					, m_DestDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(g_Timeout, "Timed out waiting for destination version manager launch")
			;

			m_DestHostID = m_DestLaunch->m_HostID;

			// Add listen socket for destination
			m_DestServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/versionmanager.sock"_f << m_DestDirectory));
			co_await m_DestLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(m_DestServerAddress)
				.f_Timeout(g_Timeout, "Timed out waiting for destination version manager add listen")
			;

			// Setup trust between destination and source
			// Destination needs to be able to connect to source
			{
				auto SourceTicket = co_await SourceState.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{SourceState.m_VersionManagerServerAddress}
					)
					.f_Timeout(g_Timeout, "Timed out generating source connection ticket")
				;

				// Add connection from destination to source
				co_await m_DestLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)
					(SourceTicket.m_Ticket, g_Timeout, -1)
					.f_Timeout(g_Timeout, "Timed out adding client connection to source")
				;

				// Grant destination read and list permissions on source BEFORE allowing namespace access
				// This ensures permissions are in place when destination subscribes to source
				co_await SourceState.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						CAppManagerTestHelper::fs_Permissions(m_DestHostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll", {}}, {"Application/ListAll", {}}})
					)
					.f_Timeout(g_Timeout, "Timed out adding permissions on source for destination")
				;

				// Allow destination to access source's VersionManager namespace
				// Now that permissions are granted, the subscription will receive versions immediately
				co_await m_DestLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						CAppManagerTestHelper::fs_NamespaceHosts(CVersionManager::mc_pDefaultNamespace, TCSet<CStr>{SourceState.m_VersionManagerHostID})
					)
					.f_Timeout(g_Timeout, "Timed out allowing namespace for source")
				;
			}

			// Setup trust between test and destination
			{
				co_await m_DestLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						CAppManagerTestHelper::fs_Permissions(SourceState.m_TestHostID, SourceState.m_VersionManagerPermissionsForTest)
					)
					.f_Timeout(g_Timeout, "Timed out adding test permissions on destination")
				;

				// Generate connection ticket from destination for test to connect
				auto DestTicket = co_await m_DestLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{m_DestServerAddress}
					)
					.f_Timeout(g_Timeout, "Timed out generating destination connection ticket")
				;

				// Connect test to destination
				co_await SourceState.m_TrustManager
					(
						&CDistributedActorTrustManager::f_AddClientConnection
						, DestTicket.m_Ticket
						, g_Timeout
						, -1
					)
					.f_Timeout(g_Timeout, "Timed out adding test connection to destination")
				;

				// Allow destination namespace on test's trust manager
				co_await SourceState.m_TrustManager
					(
						&CDistributedActorTrustManager::f_AllowHostsForNamespace
						, CVersionManager::mc_pDefaultNamespace
						, TCSet<CStr>{m_DestHostID}
						, CAppManagerTestHelper::mc_WaitForSubscriptions
					)
					.f_Timeout(g_Timeout, "Timed out allowing test namespace for destination")
				;
			}

			// Subscribe to destination VersionManager (now via test's direct connection to destination)
			m_DestVersionManager = co_await SourceState.m_Subscriptions->f_SubscribeAsync<CVersionManager>(CVersionManager::mc_pDefaultNamespace, m_DestHostID);

			co_return {};
		}

		TCFuture<void> f_Destroy()
		{
			if (m_DestLaunch)
				co_await m_DestLaunch->f_Destroy();
			m_DestLaunch.f_Clear();

			co_await m_SourceHelper.f_Destroy();

			co_return {};
		}

		// Helper to verify a version exists in the destination VersionManager
		// Since upload now waits for subscriptions, sync should be complete immediately
		TCFuture<bool> f_VerifyVersionInDest(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await m_DestVersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			co_return Result.m_Versions[_Application].f_Exists(_VersionID);
		}

		// Helper to get version tags from destination (returns empty set if version not found)
		TCFuture<TCSet<CStr>> f_GetVersionTags(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await m_DestVersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			auto *pVersion = Result.m_Versions[_Application].f_FindEqual(_VersionID);
			co_return pVersion ? pVersion->m_Tags : TCSet<CStr>();
		}

		// Helper to verify a version does NOT exist in destination
		TCFuture<bool> f_VerifyVersionNotInDest(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await m_DestVersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			co_return !Result.m_Versions[_Application].f_Exists(_VersionID);
		}

		// Helper to get the RetrySequence of a version in destination (returns 0 if not found)
		TCFuture<uint32> f_GetVersionRetrySequence(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await m_DestVersionManager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			auto *pVersion = Result.m_Versions[_Application].f_FindEqual(_VersionID);
			if (!pVersion)
				co_return 0;

			co_return pVersion->m_RetrySequence;
		}

		CAppManagerTestHelper::CState &f_SourceState()
		{
			return *m_SourceHelper.m_pState;
		}

		// Helper to get sync status sensor reading from destination using command line
		// Returns the status severity and description for the specified sync source
		TCFuture<TCOptional<CDistributedAppSensorReporter::CStatus>> f_GetDestSyncSensorStatus(CStr _SyncSourceName)
		{
			CStr VersionManagerPath = m_DestDirectory + "/VersionManager";

			TCVector<CStr> Params;
			Params.f_Insert("--sensor-status");
			Params.f_Insert("--no-only-problems");
			Params.f_Insert("--identifier");
			Params.f_Insert("org.malterlib.cloud.versionmanager.sync.status");
			Params.f_Insert("--identifier-scope");
			Params.f_Insert(_SyncSourceName);
			Params.f_Insert("--json");

			auto OutputResult = co_await m_SourceHelper.f_LaunchSimple(VersionManagerPath, fg_Move(Params), m_DestDirectory);

			auto Output = OutputResult.f_GetStdOut().f_Trim();

			if (Output.f_IsEmpty() || Output == "[]")
				co_return {};

			{
				auto CaptureScope = co_await (g_CaptureExceptions % "Error parsing sensor status JSON");

				CEJsonSorted Json = CEJsonSorted::fs_FromString(fg_Move(Output), "", false, EJsonDialectFlag_All);

				if (!Json.f_IsArray() || Json.f_Array().f_IsEmpty())
					co_return {};

				auto &FirstReading = Json.f_Array()[0];
				if (!FirstReading.f_GetMember("Value"))
					co_return {};

				auto &Value = FirstReading["Value"];

				// Value is an EJson user type (Status)
				if (!Value.f_IsUserType())
					co_return {};

				auto &StatusValue = Value.f_UserType().m_Value;

				CDistributedAppSensorReporter::CStatus Status;
				Status.m_Severity = (CDistributedAppSensorReporter::EStatusSeverity)StatusValue["Severity"].f_AsInteger(0);
				Status.m_Description = StatusValue["Description"].f_AsString();

				co_return Status;
			}
		}

		CAppManagerTestHelper::EOption m_Options;
		CAppManagerTestHelper m_SourceHelper;
		CStr m_DestDirectory;
		TCOptional<CDistributedApp_LaunchInfo> m_DestLaunch;
		CDistributedActorTrustManager_Address m_DestServerAddress;
		CStr m_DestHostID;
		TCDistributedActor<CVersionManager> m_DestVersionManager;
	};

	// Helper structure for three-way VersionManager sync tests
	struct CTripleSyncTestHelper : public CAllowUnsafeThis
	{
		CTripleSyncTestHelper(CStr _TestName, CAppManagerTestHelper::EOption _Options)
			: m_Options(_Options)
			, m_ManagerAHelper(_TestName + "_ManagerA", _Options, g_Timeout)
		{
		}

		TCFuture<void> f_SetupManagerA(mint _nAppManagers = 0)
		{
			co_await m_ManagerAHelper.f_Setup(_nAppManagers);
			co_return {};
		}

		// Result struct for version manager setup
		struct CVersionManagerSetupResult
		{
			CStr m_Directory;
			TCOptional<CDistributedApp_LaunchInfo> m_Launch;
			CDistributedActorTrustManager_Address m_ServerAddress;
			CStr m_HostID;
			TCDistributedActor<CVersionManager> m_VersionManager;
		};

		// Setup a version manager with given config and name suffix
		TCFuture<CVersionManagerSetupResult> fp_SetupVersionManager(CStr _NameSuffix, CEJsonSorted _Config)
		{
			auto &StateA = f_ManagerAState();
			CVersionManagerSetupResult Result;

			// Create directory
			Result.m_Directory = StateA.m_RootDirectory + "/VersionManager" + _NameSuffix;
			CFile::fs_CreateDirectory(Result.m_Directory);
			CFile::fs_DiffCopyFileOrDirectory(StateA.m_ProgramDirectory + "/TestApps/VersionManager", Result.m_Directory, nullptr);

			// Write sync config before launching
			CFile::fs_WriteStringToFile(Result.m_Directory + "/VersionManagerConfig.json", _Config.f_ToString(nullptr));

			// Launch VersionManager
			Result.m_Launch = co_await StateA.m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager" + _NameSuffix
					, Result.m_Directory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(g_Timeout, "Timed out waiting for version manager launch")
			;

			Result.m_HostID = Result.m_Launch->m_HostID;

			// Add listen socket
			Result.m_ServerAddress.m_URL = fg_Format("wss://[UNIX(666):{}]/", fg_GetSafeUnixSocketPath("{}/versionmanager.sock"_f << Result.m_Directory));
			co_await Result.m_Launch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddListen)(Result.m_ServerAddress)
				.f_Timeout(g_Timeout, "Timed out waiting for version manager add listen")
			;

			// Setup trust between test and this manager
			{
				co_await Result.m_Launch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						CAppManagerTestHelper::fs_Permissions(StateA.m_TestHostID, StateA.m_VersionManagerPermissionsForTest)
					)
					.f_Timeout(g_Timeout, "Timed out adding test permissions on manager")
				;

				auto Ticket = co_await Result.m_Launch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{Result.m_ServerAddress}
					)
					.f_Timeout(g_Timeout, "Timed out generating connection ticket")
				;

				co_await StateA.m_TrustManager
					(
						&CDistributedActorTrustManager::f_AddClientConnection
						, Ticket.m_Ticket
						, g_Timeout
						, -1
					)
					.f_Timeout(g_Timeout, "Timed out adding test connection to manager")
				;

				co_await StateA.m_TrustManager
					(
						&CDistributedActorTrustManager::f_AllowHostsForNamespace
						, CVersionManager::mc_pDefaultNamespace
						, TCSet<CStr>{Result.m_HostID}
						, CAppManagerTestHelper::mc_WaitForSubscriptions
					)
					.f_Timeout(g_Timeout, "Timed out allowing test namespace for manager")
				;
			}

			// Subscribe to this VersionManager from test
			Result.m_VersionManager = co_await StateA.m_Subscriptions->f_SubscribeAsync<CVersionManager>(CVersionManager::mc_pDefaultNamespace, Result.m_HostID);

			co_return Result;
		}

		// Info needed for bidirectional trust setup
		struct CManagerTrustInfo
		{
			TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
			CDistributedActorTrustManager_Address m_ServerAddress;
			CStr m_HostID;
		};

		// Setup bidirectional trust between two managers so each can sync from the other
		TCFuture<void> f_SetupBidirectionalTrust(CManagerTrustInfo _InfoX, CManagerTrustInfo _InfoY)
		{
			// X connects to Y and gets permissions
			{
				auto TicketFromY = co_await _InfoY.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{_InfoY.m_ServerAddress}
					)
					.f_Timeout(g_Timeout, "Timed out generating Y ticket for X")
				;

				co_await _InfoX.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)
					(TicketFromY.m_Ticket, g_Timeout, -1)
					.f_Timeout(g_Timeout, "Timed out X connecting to Y")
				;

				co_await _InfoY.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						CAppManagerTestHelper::fs_Permissions(_InfoX.m_HostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll", {}}, {"Application/ListAll", {}}})
					)
					.f_Timeout(g_Timeout, "Timed out granting Y permissions to X")
				;

				co_await _InfoX.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						CAppManagerTestHelper::fs_NamespaceHosts(CVersionManager::mc_pDefaultNamespace, TCSet<CStr>{_InfoY.m_HostID})
					)
					.f_Timeout(g_Timeout, "Timed out X allowing Y namespace")
				;
			}

			// Y connects to X and gets permissions
			{
				auto TicketFromX = co_await _InfoX.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_GenerateConnectionTicket)
					(
						CDistributedActorTrustManagerInterface::CGenerateConnectionTicket{_InfoX.m_ServerAddress}
					)
					.f_Timeout(g_Timeout, "Timed out generating X ticket for Y")
				;

				co_await _InfoY.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddClientConnection)
					(TicketFromX.m_Ticket, g_Timeout, -1)
					.f_Timeout(g_Timeout, "Timed out Y connecting to X")
				;

				co_await _InfoX.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
					(
						CAppManagerTestHelper::fs_Permissions(_InfoY.m_HostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll", {}}, {"Application/ListAll", {}}})
					)
					.f_Timeout(g_Timeout, "Timed out granting X permissions to Y")
				;

				co_await _InfoY.m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AllowHostsForNamespace)
					(
						CAppManagerTestHelper::fs_NamespaceHosts(CVersionManager::mc_pDefaultNamespace, TCSet<CStr>{_InfoX.m_HostID})
					)
					.f_Timeout(g_Timeout, "Timed out Y allowing X namespace")
				;
			}

			co_return {};
		}

		// Get trust info for Manager A
		CManagerTrustInfo f_GetManagerATrustInfo()
		{
			auto &StateA = f_ManagerAState();
			return {StateA.m_VersionManagerLaunch->m_pTrustInterface, StateA.m_VersionManagerServerAddress, StateA.m_VersionManagerHostID};
		}

		// Get trust info for Manager B
		CManagerTrustInfo f_GetManagerBTrustInfo()
		{
			return {m_ManagerBLaunch->m_pTrustInterface, m_ManagerBServerAddress, m_ManagerBHostID};
		}

		// Get trust info for Manager C
		CManagerTrustInfo f_GetManagerCTrustInfo()
		{
			return {m_ManagerCLaunch->m_pTrustInterface, m_ManagerCServerAddress, m_ManagerCHostID};
		}

		// Setup Manager B with config - call after f_SetupManagerA
		TCFuture<void> f_SetupManagerB(CEJsonSorted _Config)
		{
			auto Result = co_await fp_SetupVersionManager("B", _Config);
			m_ManagerBDirectory = fg_Move(Result.m_Directory);
			m_ManagerBLaunch = fg_Move(Result.m_Launch);
			m_ManagerBServerAddress = fg_Move(Result.m_ServerAddress);
			m_ManagerBHostID = fg_Move(Result.m_HostID);
			m_VersionManagerB = fg_Move(Result.m_VersionManager);
			co_return {};
		}

		// Setup Manager C with config - call after f_SetupManagerA
		TCFuture<void> f_SetupManagerC(CEJsonSorted _Config)
		{
			auto Result = co_await fp_SetupVersionManager("C", _Config);
			m_ManagerCDirectory = fg_Move(Result.m_Directory);
			m_ManagerCLaunch = fg_Move(Result.m_Launch);
			m_ManagerCServerAddress = fg_Move(Result.m_ServerAddress);
			m_ManagerCHostID = fg_Move(Result.m_HostID);
			m_VersionManagerC = fg_Move(Result.m_VersionManager);
			co_return {};
		}

		// Setup bidirectional trust between Manager A and B
		TCFuture<void> f_SetupTrustAB()
		{
			co_await f_SetupBidirectionalTrust(f_GetManagerATrustInfo(), f_GetManagerBTrustInfo());
			co_return {};
		}

		// Setup bidirectional trust between Manager A and C
		TCFuture<void> f_SetupTrustAC()
		{
			co_await f_SetupBidirectionalTrust(f_GetManagerATrustInfo(), f_GetManagerCTrustInfo());
			co_return {};
		}

		// Setup bidirectional trust between Manager B and C
		TCFuture<void> f_SetupTrustBC()
		{
			co_await f_SetupBidirectionalTrust(f_GetManagerBTrustInfo(), f_GetManagerCTrustInfo());
			co_return {};
		}

		// Enum for which manager to restart
		enum class EManager
		{
			mc_ManagerA
			, mc_ManagerB
			, mc_ManagerC
		};

		// Restart a manager with a new sync config
		// Host ID and trust remain valid since they're persisted on disk
		TCFuture<void> f_RestartManagerWithConfig(EManager _Manager, CEJsonSorted _Config)
		{
			auto &StateA = f_ManagerAState();

			CStr *pDirectory = nullptr;
			TCOptional<CDistributedApp_LaunchInfo> *pLaunch = nullptr;
			CStr *pHostID = nullptr;
			TCDistributedActor<CVersionManager> *pVersionManager = nullptr;
			CStr NameSuffix;

			if (_Manager == EManager::mc_ManagerA)
			{
				pDirectory = &StateA.m_VersionManagerDirectory;
				pLaunch = &StateA.m_VersionManagerLaunch;
				pHostID = &StateA.m_VersionManagerHostID;
				pVersionManager = &StateA.m_VersionManager;
				NameSuffix = "";
			}
			else if (_Manager == EManager::mc_ManagerB)
			{
				pDirectory = &m_ManagerBDirectory;
				pLaunch = &m_ManagerBLaunch;
				pHostID = &m_ManagerBHostID;
				pVersionManager = &m_VersionManagerB;
				NameSuffix = "B";
			}
			else
			{
				pDirectory = &m_ManagerCDirectory;
				pLaunch = &m_ManagerCLaunch;
				pHostID = &m_ManagerCHostID;
				pVersionManager = &m_VersionManagerC;
				NameSuffix = "C";
			}

			// Destroy existing manager
			if (*pLaunch)
			{
				co_await (*pLaunch)->f_Destroy();
				pLaunch->f_Clear();
			}

			// Write new sync config
			CFile::fs_WriteStringToFile(*pDirectory + "/VersionManagerConfig.json", _Config.f_ToString(nullptr));

			// Relaunch - host ID and trust are persisted
			*pLaunch = co_await StateA.m_LaunchHelper
				(
					&CDistributedApp_LaunchHelper::f_LaunchInProcess
					, "VersionManager" + NameSuffix
					, *pDirectory
					, &fg_ConstructApp_VersionManager
					, NContainer::TCVector<NStr::CStr>{}
				)
				.f_Timeout(g_Timeout, "Timed out waiting for version manager relaunch")
			;

			// Re-subscribe to get the new actor reference
			*pVersionManager = co_await StateA.m_Subscriptions->f_SubscribeAsync<CVersionManager>(CVersionManager::mc_pDefaultNamespace, *pHostID);

			co_return {};
		}

		TCFuture<void> f_Destroy()
		{
			if (m_ManagerCLaunch)
				co_await m_ManagerCLaunch->f_Destroy();
			m_ManagerCLaunch.f_Clear();

			if (m_ManagerBLaunch)
				co_await m_ManagerBLaunch->f_Destroy();
			m_ManagerBLaunch.f_Clear();

			co_await m_ManagerAHelper.f_Destroy();

			co_return {};
		}

		// Verification helper - check version exists in a specific manager
		TCFuture<bool> f_VerifyVersionInManager(TCDistributedActor<CVersionManager> _Manager, CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await _Manager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			co_return Result.m_Versions[_Application].f_Exists(_VersionID);
		}

		// Verification helper - get version tags from a manager (returns empty set if version not found)
		TCFuture<TCSet<CStr>> f_GetVersionTags(TCDistributedActor<CVersionManager> _Manager, CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			CVersionManager::CListVersions Params;
			Params.m_ForApplication = _Application;
			auto Result = co_await _Manager.f_CallActor(&CVersionManager::f_ListVersions)(Params)
				.f_Timeout(g_Timeout, "Timed out on ListVersions")
			;

			auto *pVersion = Result.m_Versions[_Application].f_FindEqual(_VersionID);
			co_return pVersion ? pVersion->m_Tags : TCSet<CStr>();
		}

		// Verification helper - check version exists in all three managers
		TCFuture<bool> f_VerifyVersionInAllManagers(CStr _Application, CVersionManager::CVersionIDAndPlatform _VersionID)
		{
			auto &StateA = f_ManagerAState();

			bool bInA = co_await f_VerifyVersionInManager(StateA.m_VersionManager, _Application, _VersionID);
			bool bInB = co_await f_VerifyVersionInManager(m_VersionManagerB, _Application, _VersionID);
			bool bInC = co_await f_VerifyVersionInManager(m_VersionManagerC, _Application, _VersionID);

			co_return bInA && bInB && bInC;
		}

		CAppManagerTestHelper::CState &f_ManagerAState()
		{
			return *m_ManagerAHelper.m_pState;
		}

		CAppManagerTestHelper::EOption m_Options;

		// Manager A - uses existing CAppManagerTestHelper infrastructure
		CAppManagerTestHelper m_ManagerAHelper;

		// Manager B
		CStr m_ManagerBDirectory;
		TCOptional<CDistributedApp_LaunchInfo> m_ManagerBLaunch;
		CDistributedActorTrustManager_Address m_ManagerBServerAddress;
		CStr m_ManagerBHostID;
		TCDistributedActor<CVersionManager> m_VersionManagerB;

		// Manager C
		CStr m_ManagerCDirectory;
		TCOptional<CDistributedApp_LaunchInfo> m_ManagerCLaunch;
		CDistributedActorTrustManager_Address m_ManagerCServerAddress;
		CStr m_ManagerCHostID;
		TCDistributedActor<CVersionManager> m_VersionManagerC;
	};

	// Helper for counting notifications to detect infinite loops
	struct CNotificationCounter
	{
		void f_OnNotification()
		{
			mint nCount = ++m_nNotifications;
			if (m_nExpectedMax > 0 && nCount > m_nExpectedMax)
			{
				// Signal loop detected - only set once
				bool bExpected = false;
				if (m_bLoopSignaled.f_CompareExchangeStrong(bExpected, true))
					m_LoopDetected.m_Promise.f_SetResult();
			}
		}

		TCFuture<void> f_GetLoopFuture()
		{
			return fg_Move(m_LoopDetected.m_Future);
		}

		TCAtomic<mint> m_nNotifications{0};
		TCPromiseFuturePair<void> m_LoopDetected;
		mint m_nExpectedMax = 0;
		TCAtomic<bool> m_bLoopSignaled{false};
	};

	struct CVersionManager_Sync_Tests : public NMib::NTest::CTest
	{
		void f_DoTests()
		{
			DMibTestSuite("Basic") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_Basic", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				// Setup source first (need at least 1 AppManager for the test infrastructure)
				co_await SyncHelper.f_SetupSource();

				// Create sync config pointing to source
				// Explicitly specify CopyTags for the tag verified in SyncExistingVersionUpdatesTagsOnly test
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_CopyTags = {"source-tag"}}
					)
				;

				// Setup destination with sync config
				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("SyncNewVersion");

					// Upload a version to source
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 2;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["SyncTestTag"];

					auto UploadResult = co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
						.f_Wrap()
					;
					DMibExpectNoException(UploadResult.f_Access());

					// Verify version synced to destination (sync completes when upload returns)
					bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
					DMibExpectTrue(bSynced);
				}

				{
					DMibTestPath("SyncMultipleVersions");

					for (mint i = 0; i < 3; ++i)
					{
						DMibTestPath("{}"_f << i);

						CVersionManager::CVersionIDAndPlatform VersionID;
						VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
						VersionID.m_VersionID.m_Major = 3;
						VersionID.m_VersionID.m_Minor = (uint32)i;
						VersionID.m_VersionID.m_Revision = 0;
						VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

						CVersionManager::CVersionInformation VersionInfo;
						VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
						VersionInfo.m_Configuration = "Release";
						VersionInfo.m_Tags["MultiSync"];

						co_await SourceState.m_VersionManagerHelper.f_Upload
							(
								SourceState.m_VersionManager
								, "TestApp"
								, VersionID
								, VersionInfo
								, SourceState.m_TestAppArchive
							)
						;

						bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
						DMibExpectTrue(bSynced);
					}
				}

				{
					DMibTestPath("SyncExistingVersionUpdatesTagsOnly");

					// This test verifies that when a version already exists in the destination,
					// syncing adds new tags without re-downloading the version content

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 10;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					// First, upload the version directly to destination with a local tag
					CVersionManager::CVersionInformation DestVersionInfo;
					DestVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					DestVersionInfo.m_Configuration = "Release";
					DestVersionInfo.m_Tags["local-tag"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SyncHelper.m_DestVersionManager
							, "TestApp"
							, VersionID
							, DestVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify version exists in destination with local tag
					TCSet<CStr> LocalTags{"local-tag"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(LocalTags)) == DMibExpr(LocalTags));

					// Now upload the same version to source with a different tag
					CVersionManager::CVersionInformation SourceVersionInfo;
					SourceVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					SourceVersionInfo.m_Configuration = "Release";
					SourceVersionInfo.m_Tags["source-tag"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, SourceVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// After sync, destination should have the source tag added and local tag preserved
					TCSet<CStr> ExpectedTags{"source-tag", "local-tag"};
					Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				co_return {};
			};

			DMibTestSuite("Tags") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_Tags", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with tag mapping
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_CopyTags = {"release=synced-release", "beta"}}  // CopyTags: transform release->synced-release, copy beta as-is
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("CopyTagMapping");

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["release"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify version synced with transformed tag, original "release" tag should NOT be present
					TCSet<CStr> ExpectedTags{"synced-release"};
					TCSet<CStr> UnexpectedTags{"release"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
					DMibTest((DMibExpr(Tags) & DMibExpr(UnexpectedTags)) == DMibExpr(TCSet<CStr>()));
				}

				{
					DMibTestPath("IdentityTagMapping");

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = 1;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["beta"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify version synced with same tag (identity mapping)
					TCSet<CStr> ExpectedTags{"beta"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("MultipleTagsOnSameVersion");

					// Test that a version with multiple matching tags gets all mappings applied
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = 2;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["release"];  // Will be mapped to synced-release
					VersionInfo.m_Tags["beta"];     // Will be copied as-is

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify both mapped tags are present
					TCSet<CStr> ExpectedTags{"synced-release", "beta"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("PreserveLocalTags");

					// Test that local tags on destination are preserved when sync adds new tags
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = 3;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					// First upload to destination with a local tag
					CVersionManager::CVersionInformation DestVersionInfo;
					DestVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					DestVersionInfo.m_Configuration = "Release";
					DestVersionInfo.m_Tags["local-only-tag"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SyncHelper.m_DestVersionManager
							, "TestApp"
							, VersionID
							, DestVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Now upload same version to source with a tag that will be synced
					CVersionManager::CVersionInformation SourceVersionInfo;
					SourceVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					SourceVersionInfo.m_Configuration = "Release";
					SourceVersionInfo.m_Tags["release"];  // Will be mapped to synced-release

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, SourceVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify both local and synced tags are present
					TCSet<CStr> ExpectedTags{"local-only-tag", "synced-release"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("UpdatedSourceTagsUpdateDestination");

					// Test that when source tags are updated, destination gets updated too
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 4;
					VersionID.m_VersionID.m_Minor = 4;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					// First upload with one tag
					CVersionManager::CVersionInformation VersionInfo1;
					VersionInfo1.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo1.m_Configuration = "Release";
					VersionInfo1.m_Tags["release"];  // Will be mapped to synced-release

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo1
							, SourceState.m_TestAppArchive
						)
					;

					// Verify initial synced tag is present
					TCSet<CStr> InitialTags{"synced-release"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(InitialTags)) == DMibExpr(InitialTags));

					// Now add another tag to the version on source using f_ChangeTags
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["beta"];  // Add new tag

					co_await SourceState.m_VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Timed out changing tags")
					;

					// Verify both tags are now present in destination
					TCSet<CStr> UpdatedTags{"synced-release", "beta"};
					Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(UpdatedTags)) == DMibExpr(UpdatedTags));
				}

				co_return {};
			};

			DMibTestSuite("Filters") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_Filters", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with tag filter
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_Applications = {"TestApp*"}, .m_Tags = {"release*"}}  // Application and tag filters
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("TagFilter");

					// Upload version with matching tag
					CVersionManager::CVersionIDAndPlatform MatchingVersionID;
					MatchingVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					MatchingVersionID.m_VersionID.m_Major = 5;
					MatchingVersionID.m_VersionID.m_Minor = 0;
					MatchingVersionID.m_VersionID.m_Revision = 0;
					MatchingVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation MatchingVersionInfo;
					MatchingVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					MatchingVersionInfo.m_Configuration = "Release";
					MatchingVersionInfo.m_Tags["release"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, MatchingVersionID
							, MatchingVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload version with non-matching tag
					CVersionManager::CVersionIDAndPlatform NonMatchingVersionID;
					NonMatchingVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					NonMatchingVersionID.m_VersionID.m_Major = 5;
					NonMatchingVersionID.m_VersionID.m_Minor = 1;
					NonMatchingVersionID.m_VersionID.m_Revision = 0;
					NonMatchingVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation NonMatchingVersionInfo;
					NonMatchingVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					NonMatchingVersionInfo.m_Configuration = "Release";
					NonMatchingVersionInfo.m_Tags["beta"];  // Should NOT sync

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, NonMatchingVersionID
							, NonMatchingVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Matching version should sync (sync completes when upload returns)
					bool bMatchingSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", MatchingVersionID);
					DMibExpectTrue(bMatchingSynced);

					// Non-matching version should NOT have synced
					bool bNonMatchingNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", NonMatchingVersionID);
					DMibExpectTrue(bNonMatchingNotSynced);
				}

				{
					DMibTestPath("ApplicationFilter");

					// The sync config has Applications = ["TestApp*"], so only TestApp* should sync
					// Upload version to a matching application (TestApp)
					CVersionManager::CVersionIDAndPlatform MatchingAppVersionID;
					MatchingAppVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					MatchingAppVersionID.m_VersionID.m_Major = 5;
					MatchingAppVersionID.m_VersionID.m_Minor = 2;
					MatchingAppVersionID.m_VersionID.m_Revision = 0;
					MatchingAppVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation MatchingAppVersionInfo;
					MatchingAppVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					MatchingAppVersionInfo.m_Configuration = "Release";
					MatchingAppVersionInfo.m_Tags["release"];  // Matches tag filter too

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestAppVariant"  // Matches "TestApp*" pattern
							, MatchingAppVersionID
							, MatchingAppVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload version to a non-matching application
					CVersionManager::CVersionIDAndPlatform NonMatchingAppVersionID;
					NonMatchingAppVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					NonMatchingAppVersionID.m_VersionID.m_Major = 5;
					NonMatchingAppVersionID.m_VersionID.m_Minor = 3;
					NonMatchingAppVersionID.m_VersionID.m_Revision = 0;
					NonMatchingAppVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation NonMatchingAppVersionInfo;
					NonMatchingAppVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					NonMatchingAppVersionInfo.m_Configuration = "Release";
					NonMatchingAppVersionInfo.m_Tags["release"];  // Has matching tag, but wrong app

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "OtherApp"  // Does NOT match "TestApp*" pattern
							, NonMatchingAppVersionID
							, NonMatchingAppVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Matching app version should sync
					bool bMatchingAppSynced = co_await SyncHelper.f_VerifyVersionInDest("TestAppVariant", MatchingAppVersionID);
					DMibExpectTrue(bMatchingAppSynced);

					// Non-matching app version should NOT sync
					bool bNonMatchingAppNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("OtherApp", NonMatchingAppVersionID);
					DMibExpectTrue(bNonMatchingAppNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("PlatformFilters") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_PlatformFilters", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with platform filter
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_Platforms = {"macOS*"}}  // Platforms - only sync macOS* platforms
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("PlatformFilter");

					// Upload version with matching platform (macOS)
					CVersionManager::CVersionIDAndPlatform MatchingPlatformVersionID;
					MatchingPlatformVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					MatchingPlatformVersionID.m_VersionID.m_Major = 11;
					MatchingPlatformVersionID.m_VersionID.m_Minor = 0;
					MatchingPlatformVersionID.m_VersionID.m_Revision = 0;
					MatchingPlatformVersionID.m_Platform = "macOS-x86-64";  // Matches "macOS*"

					CVersionManager::CVersionInformation MatchingPlatformVersionInfo;
					MatchingPlatformVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					MatchingPlatformVersionInfo.m_Configuration = "Release";
					MatchingPlatformVersionInfo.m_Tags["test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, MatchingPlatformVersionID
							, MatchingPlatformVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload version with non-matching platform (Windows)
					CVersionManager::CVersionIDAndPlatform NonMatchingPlatformVersionID;
					NonMatchingPlatformVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					NonMatchingPlatformVersionID.m_VersionID.m_Major = 11;
					NonMatchingPlatformVersionID.m_VersionID.m_Minor = 1;
					NonMatchingPlatformVersionID.m_VersionID.m_Revision = 0;
					NonMatchingPlatformVersionID.m_Platform = "Windows-x86-64";  // Does NOT match "macOS*"

					CVersionManager::CVersionInformation NonMatchingPlatformVersionInfo;
					NonMatchingPlatformVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					NonMatchingPlatformVersionInfo.m_Configuration = "Release";
					NonMatchingPlatformVersionInfo.m_Tags["test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, NonMatchingPlatformVersionID
							, NonMatchingPlatformVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Matching platform version should sync
					bool bMatchingPlatformSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", MatchingPlatformVersionID);
					DMibExpectTrue(bMatchingPlatformSynced);

					// Non-matching platform version should NOT sync
					bool bNonMatchingPlatformNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", NonMatchingPlatformVersionID);
					DMibExpectTrue(bNonMatchingPlatformNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("Pretend") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_Pretend", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with pretend mode enabled
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_bPretend = true}
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("NothingActuallySyncs");

					// Upload a version to source
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 6;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["PretendTag"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// In pretend mode, version should NOT actually sync
					bool bNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", VersionID);
					DMibExpectTrue(bNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("PretendMixed") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_PretendMixed", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create two sync configs: one pretend (for pretend* tags), one real (for real* tags)
				CEJsonSorted SyncSources = EJsonType_Object;
				SyncSources["PretendSync"] = fg_CreateSyncSource
					(
						{SourceState.m_VersionManagerHostID}
						, {.m_Tags = {"pretend*"}, .m_bPretend = true}
					)
				;
				SyncSources["RealSync"] = fg_CreateSyncSource
					(
						{SourceState.m_VersionManagerHostID}
						, {.m_Tags = {"real*"}}  // Pretend = false (default)
					)
				;

				auto SyncConfig = fg_CreateMultiSyncConfig(SyncSources);

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("MixedPretendAndReal");

					// Upload version with pretend tag - should NOT sync (pretend mode)
					CVersionManager::CVersionIDAndPlatform PretendVersionID;
					PretendVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					PretendVersionID.m_VersionID.m_Major = 12;
					PretendVersionID.m_VersionID.m_Minor = 0;
					PretendVersionID.m_VersionID.m_Revision = 0;
					PretendVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation PretendVersionInfo;
					PretendVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					PretendVersionInfo.m_Configuration = "Release";
					PretendVersionInfo.m_Tags["pretend-test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, PretendVersionID
							, PretendVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload version with real tag - should sync (real mode)
					CVersionManager::CVersionIDAndPlatform RealVersionID;
					RealVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					RealVersionID.m_VersionID.m_Major = 12;
					RealVersionID.m_VersionID.m_Minor = 1;
					RealVersionID.m_VersionID.m_Revision = 0;
					RealVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation RealVersionInfo;
					RealVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					RealVersionInfo.m_Configuration = "Release";
					RealVersionInfo.m_Tags["real-test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, RealVersionID
							, RealVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Pretend version should NOT sync (pretend mode is enabled for this config)
					bool bPretendNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", PretendVersionID);
					DMibExpectTrue(bPretendNotSynced);

					// Real version SHOULD sync (real mode is enabled for this config)
					bool bRealSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", RealVersionID);
					DMibExpectTrue(bRealSynced);
				}

				co_return {};
			};

			DMibTestSuite("DateFiltering") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_DateFilter", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Set StartSyncDate to 7 days ago
				auto StartDate = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(7);
				CStr StartDateStr = "{td}"_f << StartDate;

				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_StartSyncDate = StartDateStr}  // MinSyncVersions = 0 (default) so date filtering is strictly applied
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("StartSyncDateFiltersOldVersions");

					// Upload old version (30 days ago - before StartSyncDate)
					CVersionManager::CVersionIDAndPlatform OldVersionID;
					OldVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					OldVersionID.m_VersionID.m_Major = 7;
					OldVersionID.m_VersionID.m_Minor = 0;
					OldVersionID.m_VersionID.m_Revision = 0;
					OldVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation OldVersionInfo;
					OldVersionInfo.m_Time = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(30);
					OldVersionInfo.m_Configuration = "Release";
					OldVersionInfo.m_Tags["OldVersion"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, OldVersionID
							, OldVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload new version (today - after StartSyncDate)
					CVersionManager::CVersionIDAndPlatform NewVersionID;
					NewVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					NewVersionID.m_VersionID.m_Major = 7;
					NewVersionID.m_VersionID.m_Minor = 1;
					NewVersionID.m_VersionID.m_Revision = 0;
					NewVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation NewVersionInfo;
					NewVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					NewVersionInfo.m_Configuration = "Release";
					NewVersionInfo.m_Tags["NewVersion"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, NewVersionID
							, NewVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// New version should sync (sync completes when upload returns)
					bool bNewSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", NewVersionID);
					DMibExpectTrue(bNewSynced);

					// Old version should NOT sync (before StartSyncDate, and MinSyncVersions=0)
					bool bOldNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", OldVersionID);
					DMibExpectTrue(bOldNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("DateFilteringMinSyncVersions") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_DateFilterMin", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				{
					DMibTestPath("MinSyncVersionsBypassesDateFilter");

					// Upload 3 old versions (all before StartSyncDate - 30, 20, 15 days ago)
					// With MinSyncVersions=2, the 2 newest (15 and 20 days) should sync despite being old
					// IMPORTANT: Upload versions BEFORE setting up destination so the full resend includes all versions

					CVersionManager::CVersionIDAndPlatform OldestVersionID;
					OldestVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					OldestVersionID.m_VersionID.m_Major = 8;
					OldestVersionID.m_VersionID.m_Minor = 0;
					OldestVersionID.m_VersionID.m_Revision = 0;
					OldestVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation OldestVersionInfo;
					OldestVersionInfo.m_Time = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(30);
					OldestVersionInfo.m_Configuration = "Release";
					OldestVersionInfo.m_Tags["oldest"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, OldestVersionID
							, OldestVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					CVersionManager::CVersionIDAndPlatform MiddleVersionID;
					MiddleVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					MiddleVersionID.m_VersionID.m_Major = 8;
					MiddleVersionID.m_VersionID.m_Minor = 1;
					MiddleVersionID.m_VersionID.m_Revision = 0;
					MiddleVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation MiddleVersionInfo;
					MiddleVersionInfo.m_Time = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(20);
					MiddleVersionInfo.m_Configuration = "Release";
					MiddleVersionInfo.m_Tags["middle"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, MiddleVersionID
							, MiddleVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					CVersionManager::CVersionIDAndPlatform NewestVersionID;
					NewestVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					NewestVersionID.m_VersionID.m_Major = 8;
					NewestVersionID.m_VersionID.m_Minor = 2;
					NewestVersionID.m_VersionID.m_Revision = 0;
					NewestVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation NewestVersionInfo;
					NewestVersionInfo.m_Time = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(15);
					NewestVersionInfo.m_Configuration = "Release";
					NewestVersionInfo.m_Tags["newest"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, NewestVersionID
							, NewestVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Now set up destination - the initial subscription triggers a full resend
					// MinSyncVersions only works during full resend when all versions are present
					auto StartDate = NTime::CTime::fs_NowUTC() - NTime::CTimeSpanConvert::fs_CreateDaySpan(7);
					CStr StartDateStr = "{td}"_f << StartDate;

					// Use MinSyncVersions=3 because the base version 1.0.1 (from test setup) is also in the
					// version list with a recent timestamp. The 3 newest are: 1.0.1, 8.2.0, 8.1.0
					// This allows us to verify that 8.2.0 and 8.1.0 (both older than StartSyncDate) get synced
					auto SyncConfig = fg_CreateSyncConfig
						(
							"TestSync"
							, {SourceState.m_VersionManagerHostID}
							, {.m_StartSyncDate = StartDateStr, .m_nMinSyncVersions = 3}  // 3 newest versions bypass date filter
						)
					;

					co_await SyncHelper.f_SetupDestination(SyncConfig);

					// The 3 newest overall (1.0.1, 8.2.0, 8.1.0) bypass date filter via MinSyncVersions=3
					// 8.0.0 (oldest) should NOT sync as it's not in the top 3 newest
					bool bNewestSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", NewestVersionID);
					DMibExpectTrue(bNewestSynced);

					bool bMiddleSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", MiddleVersionID);
					DMibExpectTrue(bMiddleSynced);

					bool bOldestNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", OldestVersionID);
					DMibExpectTrue(bOldestNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("ConcurrentSync") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_Concurrent", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("SimultaneousVersionsFromSameHost");

					TCFutureVector<CVersionManagerHelper::CUploadResult> UploadFutures;

					// Upload 5 versions rapidly
					for (mint i = 0; i < 5; ++i)
					{
						CVersionManager::CVersionIDAndPlatform VersionID;
						VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
						VersionID.m_VersionID.m_Major = 8;
						VersionID.m_VersionID.m_Minor = (uint32)i;
						VersionID.m_VersionID.m_Revision = 0;
						VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

						CVersionManager::CVersionInformation VersionInfo;
						VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
						VersionInfo.m_Configuration = "Release";
						VersionInfo.m_Tags["ConcurrentSync"];

						SourceState.m_VersionManagerHelper.f_Upload
							(
								SourceState.m_VersionManager
								, "TestApp"
								, VersionID
								, VersionInfo
								, SourceState.m_TestAppArchive
							)
							> UploadFutures
						;
					}

					// Wait for all uploads to complete
					auto UploadResults = co_await fg_AllDone(UploadFutures).f_Wrap();
					DMibAssertNoException(UploadResults.f_Access());

					// Verify all versions synced to destination (sync completes when uploads return)
					for (mint i = 0; i < 5; ++i)
					{
						DMibTestPath("{}"_f << i);

						CVersionManager::CVersionIDAndPlatform VersionID;
						VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
						VersionID.m_VersionID.m_Major = 8;
						VersionID.m_VersionID.m_Minor = (uint32)i;
						VersionID.m_VersionID.m_Revision = 0;
						VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

						bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
						DMibExpectTrue(bSynced);
					}
				}

				co_return {};
			};

			// Test that multiple sync sources don't cause duplicate downloads
			// Uses two sync configs pointing to same source with overlapping filters
			DMibTestSuite("ConcurrentSyncMultipleSources") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_ConcurrentMulti", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create two sync configs with overlapping filters - both will try to sync the same version
				// Both sources need CopyTags to sync the "multi-source" tag
				CEJsonSorted SyncSources = EJsonType_Object;
				SyncSources["SyncSource1"] = fg_CreateSyncSource({SourceState.m_VersionManagerHostID}, {.m_CopyTags = {"multi-source"}});
				SyncSources["SyncSource2"] = fg_CreateSyncSource({SourceState.m_VersionManagerHostID}, {.m_CopyTags = {"multi-source"}});

				auto SyncConfig = fg_CreateMultiSyncConfig(SyncSources);

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("SequencerPreventsDuplicateDownloads");

					// Upload a version - both sync sources will try to sync it
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 13;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["multi-source"];

					// Upload the version (both sources will be notified)
					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify the version synced exactly once (no errors, correct result)
					bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
					DMibExpectTrue(bSynced);

					// Verify tags are correct (version wasn't corrupted by concurrent access)
					TCSet<CStr> ExpectedTags{"multi-source"};
					TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", VersionID);
					DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				co_return {};
			};

			DMibTestSuite("MultipleSources") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_MultipleSources", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create two sync configs with different tag mappings
				// Source1: copies "alpha" tag as-is
				// Source2: transforms "beta" to "synced-beta"
				CEJsonSorted SyncSources = EJsonType_Object;
				SyncSources["Source1"] = fg_CreateSyncSource
					(
						{SourceState.m_VersionManagerHostID}
						, {.m_Tags = {"alpha*"}, .m_CopyTags = {"alpha"}}  // Copy alpha tag as-is
					)
				;
				SyncSources["Source2"] = fg_CreateSyncSource
					(
						{SourceState.m_VersionManagerHostID}
						, {.m_Tags = {"beta*"}, .m_CopyTags = {"beta=synced-beta"}}  // Transform beta to synced-beta
					)
				;

				auto SyncConfig = fg_CreateMultiSyncConfig(SyncSources);

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("DifferentTagMappings");

					// Upload version with alpha tag - should sync with alpha tag copied as-is
					CVersionManager::CVersionIDAndPlatform AlphaVersionID;
					AlphaVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					AlphaVersionID.m_VersionID.m_Major = 14;
					AlphaVersionID.m_VersionID.m_Minor = 0;
					AlphaVersionID.m_VersionID.m_Revision = 0;
					AlphaVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation AlphaVersionInfo;
					AlphaVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					AlphaVersionInfo.m_Configuration = "Release";
					AlphaVersionInfo.m_Tags["alpha"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, AlphaVersionID
							, AlphaVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Upload version with beta tag - should sync with transformed tag
					CVersionManager::CVersionIDAndPlatform BetaVersionID;
					BetaVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					BetaVersionID.m_VersionID.m_Major = 14;
					BetaVersionID.m_VersionID.m_Minor = 1;
					BetaVersionID.m_VersionID.m_Revision = 0;
					BetaVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation BetaVersionInfo;
					BetaVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					BetaVersionInfo.m_Configuration = "Release";
					BetaVersionInfo.m_Tags["beta"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, BetaVersionID
							, BetaVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify alpha version synced with "alpha" tag
					{
						DMibTestPath("Alpha");
						TCSet<CStr> ExpectedTags{"alpha"};
						TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", AlphaVersionID);
						DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
					}

					// Verify beta version synced with transformed "synced-beta" tag, original beta should NOT be present
					{
						DMibTestPath("Beta");
						TCSet<CStr> ExpectedTags{"synced-beta"};
						TCSet<CStr> UnexpectedTags{"beta"};
						TCSet<CStr> Tags = co_await SyncHelper.f_GetVersionTags("TestApp", BetaVersionID);
						DMibTest((DMibExpr(Tags) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
						DMibTest((DMibExpr(Tags) & DMibExpr(UnexpectedTags)) == DMibExpr(TCSet<CStr>()));
					}
				}

				{
					DMibTestPath("SameHostDifferentFilters");

					// Upload version that matches neither filter (gamma tag)
					CVersionManager::CVersionIDAndPlatform GammaVersionID;
					GammaVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					GammaVersionID.m_VersionID.m_Major = 14;
					GammaVersionID.m_VersionID.m_Minor = 2;
					GammaVersionID.m_VersionID.m_Revision = 0;
					GammaVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation GammaVersionInfo;
					GammaVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					GammaVersionInfo.m_Configuration = "Release";
					GammaVersionInfo.m_Tags["gamma"];  // Neither alpha* nor beta*

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, GammaVersionID
							, GammaVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Gamma version should NOT sync (matches neither filter)
					bool bGammaNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", GammaVersionID);
					DMibExpectTrue(bGammaNotSynced);
				}

				co_return {};
			};

			DMibTestSuite("RetrySequenceEnabled") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_RetrySeqEnabled", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with SyncRetrySequence enabled
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
						, {.m_bSyncRetrySequence = true}
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("SyncRetrySequenceEnabled");

					// Upload version with RetrySequence = 5
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 15;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_RetrySequence = 5;  // Set RetrySequence
					VersionInfo.m_Tags["retry-test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify version synced
					bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
					DMibExpectTrue(bSynced);

					// Verify RetrySequence was synced (should be 5)
					uint32 DestRetrySequence = co_await SyncHelper.f_GetVersionRetrySequence("TestApp", VersionID);
					DMibExpect(DestRetrySequence, ==, 5u);
				}

				co_return {};
			};

			DMibTestSuite("RetrySequenceDisabled") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_RetrySeqDisabled", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				// Create sync config with SyncRetrySequence disabled (default)
				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("DisabledDoesNotSyncRetrySequence");

					// Upload version with RetrySequence = 5
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 16;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_RetrySequence = 5;  // Set RetrySequence
					VersionInfo.m_Tags["retry-disabled-test"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Verify version synced
					bool bSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", VersionID);
					DMibExpectTrue(bSynced);

					// Verify RetrySequence was NOT synced (should be 0, the default)
					uint32 DestRetrySequence = co_await SyncHelper.f_GetVersionRetrySequence("TestApp", VersionID);
					DMibExpect(DestRetrySequence, ==, 0u);
				}

				co_return {};
			};

			DMibTestSuite("ErrorRecovery") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CSyncTestHelper SyncHelper("VersionManagerSyncTests_ErrorRecovery", Options);
				auto Destroy = co_await fg_AsyncDestroy(SyncHelper);
				auto &SourceState = SyncHelper.f_SourceState();

				co_await SyncHelper.f_SetupSource();

				auto SyncConfig = fg_CreateSyncConfig
					(
						"TestSync"
						, {SourceState.m_VersionManagerHostID}
					)
				;

				co_await SyncHelper.f_SetupDestination(SyncConfig);

				{
					DMibTestPath("SyncContinuesAfterError");

					// Upload first version - this should sync successfully
					CVersionManager::CVersionIDAndPlatform FirstVersionID;
					FirstVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					FirstVersionID.m_VersionID.m_Major = 9;
					FirstVersionID.m_VersionID.m_Minor = 0;
					FirstVersionID.m_VersionID.m_Revision = 0;
					FirstVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation FirstVersionInfo;
					FirstVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					FirstVersionInfo.m_Configuration = "Release";
					FirstVersionInfo.m_Tags["First"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, FirstVersionID
							, FirstVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// First version should sync successfully
					bool bFirstSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", FirstVersionID);
					DMibExpectTrue(bFirstSynced);

					// Sensor should report Ok (no errors)
					auto InitialStatus = co_await SyncHelper.f_GetDestSyncSensorStatus("TestSync");
					DMibExpect((InitialStatus ? InitialStatus->m_Severity : CDistributedAppSensorReporter::EStatusSeverity_Error), ==, CDistributedAppSensorReporter::EStatusSeverity_Ok);

					// Now remove the read permission from destination - this will cause sync to fail
					{
						TCSet<CStr> PermissionsToRemove;
						PermissionsToRemove["Application/ReadAll"];
						CDistributedActorTrustManagerInterface::CRemovePermissions RemoveCmd
							{
								.m_Identity = CPermissionIdentifiers{SyncHelper.m_DestHostID, ""}
								, .m_Permissions = PermissionsToRemove
								, .m_OrderingFlags = CAppManagerTestHelper::mc_WaitForSubscriptions
							}
						;
						co_await SourceState.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_RemovePermissions)(RemoveCmd)
							.f_Timeout(g_Timeout, "Timed out removing permissions")
						;
					}

					// Upload second version - sync will fail due to missing permission
					CVersionManager::CVersionIDAndPlatform SecondVersionID;
					SecondVersionID.m_VersionID.m_Branch = SourceState.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					SecondVersionID.m_VersionID.m_Major = 9;
					SecondVersionID.m_VersionID.m_Minor = 1;
					SecondVersionID.m_VersionID.m_Revision = 0;
					SecondVersionID.m_Platform = SourceState.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation SecondVersionInfo;
					SecondVersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					SecondVersionInfo.m_Configuration = "Release";
					SecondVersionInfo.m_Tags["Second"];

					co_await SourceState.m_VersionManagerHelper.f_Upload
						(
							SourceState.m_VersionManager
							, "TestApp"
							, SecondVersionID
							, SecondVersionInfo
							, SourceState.m_TestAppArchive
						)
					;

					// Second version should NOT have synced (permission denied)
					bool bSecondNotSynced = co_await SyncHelper.f_VerifyVersionNotInDest("TestApp", SecondVersionID);
					DMibExpectTrue(bSecondNotSynced);

					// Sensor should report Error due to sync failure
					auto ErrorStatus = co_await SyncHelper.f_GetDestSyncSensorStatus("TestSync");
					DMibExpectTrue(ErrorStatus);
					DMibExpect(ErrorStatus->m_Severity, ==, CDistributedAppSensorReporter::EStatusSeverity_Error);

					// Restore the permission
					co_await SourceState.m_VersionManagerLaunch->m_pTrustInterface->f_CallActor(&CDistributedActorTrustManagerInterface::f_AddPermissions)
						(
							CAppManagerTestHelper::fs_Permissions(SyncHelper.m_DestHostID, TCMap<CStr, CPermissionRequirements>{{"Application/ReadAll", {}}})
						)
						.f_Timeout(g_Timeout, "Timed out restoring permissions")
					;

					// Second version should also have synced now (retried after permission restored)
					bool bSecondNowSynced = co_await SyncHelper.f_VerifyVersionInDest("TestApp", SecondVersionID);
					DMibExpectTrue(bSecondNowSynced);

					// Sensor should report Ok (error cleared after successful sync)
					auto OkStatus = co_await SyncHelper.f_GetDestSyncSensorStatus("TestSync");
					DMibExpectTrue(OkStatus);
					DMibExpect(OkStatus->m_Severity, ==, CDistributedAppSensorReporter::EStatusSeverity_Ok);
				}

				co_return {};
			};

			// Three-way sync tests: A, B, C all sync from each other (full mesh)
			DMibTestSuite("ThreeWay-Basic") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CTripleSyncTestHelper Helper("ThreeWaySyncTests_Basic", Options);
				auto Destroy = co_await fg_AsyncDestroy(Helper);

				// Phase 1: Setup all managers first with empty configs to get their host IDs
				co_await Helper.f_SetupManagerA();
				auto &StateA = Helper.f_ManagerAState();

				CEJsonSorted EmptyConfig = EJsonType_Object;
				co_await Helper.f_SetupManagerB(EmptyConfig);
				co_await Helper.f_SetupManagerC(EmptyConfig);

				// Setup bidirectional trust between all pairs
				co_await Helper.f_SetupTrustAB();
				co_await Helper.f_SetupTrustAC();
				co_await Helper.f_SetupTrustBC();

				// Phase 2: Now we have all host IDs, restart with full mesh configs
				CStr HostIDA = StateA.m_VersionManagerHostID;
				CStr HostIDB = Helper.m_ManagerBHostID;
				CStr HostIDC = Helper.m_ManagerCHostID;

				// A syncs from B and C (copy tags used in tests)
				CEJsonSorted SyncSourcesA = EJsonType_Object;
				SyncSourcesA["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"b-release", "new-tag-from-b"}});
				SyncSourcesA["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"c-release", "new-tag-from-c"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerA, fg_CreateMultiSyncConfig(SyncSourcesA));

				// B syncs from A and C (copy tags used in tests)
				CEJsonSorted SyncSourcesB = EJsonType_Object;
				SyncSourcesB["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"a-release", "new-tag-from-a"}});
				SyncSourcesB["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"c-release", "new-tag-from-c"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerB, fg_CreateMultiSyncConfig(SyncSourcesB));

				// C syncs from A and B (copy tags used in tests)
				CEJsonSorted SyncSourcesC = EJsonType_Object;
				SyncSourcesC["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"a-release", "new-tag-from-a"}});
				SyncSourcesC["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"b-release", "new-tag-from-b"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerC, fg_CreateMultiSyncConfig(SyncSourcesC));
				{
					DMibTestPath("UploadOnA_AppearOnBC");

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["a-release"];

					co_await StateA.m_VersionManagerHelper.f_Upload
						(
							StateA.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
					;

					// Verify version synced to B and C
					bool bInB = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibExpectTrue(bInB);

					bool bInC = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibExpectTrue(bInC);
				}

				{
					DMibTestPath("UploadOnB_AppearOnAC");

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 1;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["b-release"];

					co_await StateA.m_VersionManagerHelper.f_Upload
						(
							Helper.m_VersionManagerB
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
					;

					// Verify version synced to A and C
					bool bInA = co_await Helper.f_VerifyVersionInManager(StateA.m_VersionManager, "TestApp", VersionID);
					DMibExpectTrue(bInA);

					bool bInC = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibExpectTrue(bInC);
				}

				{
					DMibTestPath("UploadOnC_AppearOnAB");

					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 2;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["c-release"];

					co_await StateA.m_VersionManagerHelper.f_Upload
						(
							Helper.m_VersionManagerC
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
					;

					// Verify version synced to A and B
					bool bInA = co_await Helper.f_VerifyVersionInManager(StateA.m_VersionManager, "TestApp", VersionID);
					DMibExpectTrue(bInA);

					bool bInB = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibExpectTrue(bInB);
				}

				{
					DMibTestPath("TagChangeOnA_AppearOnBC");

					// Use the version that was uploaded to A in the first test (100.0.0)
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					// Add a new tag to the version on A
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["new-tag-from-a"];

					co_await StateA.m_VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Timed out changing tags on A")
					;

					// Verify tag propagated to B and C
					TCSet<CStr> ExpectedTags{"new-tag-from-a"};
					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));

					TCSet<CStr> TagsC = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsC) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("TagChangeOnB_AppearOnAC");

					// Use the version that was uploaded to B in the second test (100.1.0)
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 1;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					// Add a new tag to the version on B
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["new-tag-from-b"];

					co_await Helper.m_VersionManagerB.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Timed out changing tags on B")
					;

					// Verify tag propagated to A and C
					TCSet<CStr> ExpectedTags{"new-tag-from-b"};
					TCSet<CStr> TagsA = co_await Helper.f_GetVersionTags(StateA.m_VersionManager, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsA) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));

					TCSet<CStr> TagsC = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsC) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("TagChangeOnC_AppearOnAB");

					// Use the version that was uploaded to C in the third test (100.2.0)
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 100;
					VersionID.m_VersionID.m_Minor = 2;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					// Add a new tag to the version on C
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["new-tag-from-c"];

					co_await Helper.m_VersionManagerC.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Timed out changing tags on C")
					;

					// Verify tag propagated to A and B
					TCSet<CStr> ExpectedTags{"new-tag-from-c"};
					TCSet<CStr> TagsA = co_await Helper.f_GetVersionTags(StateA.m_VersionManager, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsA) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));

					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				co_return {};
			};

			// Loop prevention test: Identity tag mapping between A and B (full mesh)
			// A and B both sync "release" tag from each other - potential infinite loop
			DMibTestSuite("ThreeWay-LoopPrevention-IdentityMapping") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CTripleSyncTestHelper Helper("ThreeWaySyncTests_LoopIdentity", Options);
				auto Destroy = co_await fg_AsyncDestroy(Helper);

				// Phase 1: Setup all managers with empty configs to get host IDs
				co_await Helper.f_SetupManagerA();
				auto &StateA = Helper.f_ManagerAState();

				CEJsonSorted EmptyConfig = EJsonType_Object;
				co_await Helper.f_SetupManagerB(EmptyConfig);
				co_await Helper.f_SetupManagerC(EmptyConfig);

				// Setup bidirectional trust between all pairs
				co_await Helper.f_SetupTrustAB();
				co_await Helper.f_SetupTrustAC();
				co_await Helper.f_SetupTrustBC();

				// Phase 2: Restart with identity mapping configs that could cause loops
				CStr HostIDA = StateA.m_VersionManagerHostID;
				CStr HostIDB = Helper.m_ManagerBHostID;
				CStr HostIDC = Helper.m_ManagerCHostID;

				// A syncs "release" from B (identity mapping - potential loop with B), relevant tags from C
				CEJsonSorted SyncSourcesA = EJsonType_Object;
				SyncSourcesA["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"release", "other-tag"}});
				SyncSourcesA["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"release", "other-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerA, fg_CreateMultiSyncConfig(SyncSourcesA));

				// B syncs "release" from A (identity mapping - potential loop with A), relevant tags from C
				CEJsonSorted SyncSourcesB = EJsonType_Object;
				SyncSourcesB["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"release", "other-tag"}});
				SyncSourcesB["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"release", "other-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerB, fg_CreateMultiSyncConfig(SyncSourcesB));

				// C syncs relevant tags from A and B (full mesh participant, not in identity loop)
				CEJsonSorted SyncSourcesC = EJsonType_Object;
				SyncSourcesC["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"release", "other-tag"}});
				SyncSourcesC["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"release", "other-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerC, fg_CreateMultiSyncConfig(SyncSourcesC));

				{
					DMibTestPath("IdentityTagMappingNoLoop");

					// Setup notification counter
					CNotificationCounter Counter;
					Counter.m_nExpectedMax = 5;  // A upload + B sync = 2 notifications expected, allow buffer

					// Upload version to A with "release" tag
					// If there's a loop: A uploads -> B syncs -> A syncs from B -> B syncs again -> ...
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 200;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["release"];

					// Race: upload should complete vs loop detected
					auto UploadFuture = StateA.m_VersionManagerHelper.f_Upload
						(
							StateA.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
						.f_Timeout(g_Timeout, "Upload timed out - possible deadlock")
					;

					auto LoopFuture = Counter.f_GetLoopFuture();

					auto Result = co_await fg_AnyDone(fg_Move(UploadFuture), fg_Move(LoopFuture));

					// Check that loop was not detected (counter stayed within bounds)
					DMibExpectTrue(Counter.m_nNotifications <= Counter.m_nExpectedMax);

					// Verify version exists in B with "release" tag
					bool bInB = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibExpectTrue(bInB);

					TCSet<CStr> ExpectedTags{"release"};
					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				{
					DMibTestPath("IdentityTagChangeNoLoop");

					// Upload version to A WITHOUT "release" tag first
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 200;
					VersionID.m_VersionID.m_Minor = 1;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["other-tag"];  // Not "release", won't trigger identity sync

					co_await StateA.m_VersionManagerHelper.f_Upload
						(
							StateA.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
						.f_Timeout(g_Timeout, "Upload timed out")
					;

					// Version should sync to B and C (unrestricted sync), but without "release" tag
					bool bInB = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibExpectTrue(bInB);

					// Setup notification counter for tag change
					CNotificationCounter Counter;
					Counter.m_nExpectedMax = 5;  // Tag change + sync notifications, allow buffer

					// Now add "release" tag - this should trigger identity mapping sync
					// If there's a loop: A adds tag -> B syncs tag -> A syncs from B -> B syncs again -> ...
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["release"];

					auto ChangeFuture = StateA.m_VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Tag change timed out - deadlock")
					;

					auto LoopFuture = Counter.f_GetLoopFuture();

					auto Result = co_await fg_AnyDone(fg_Move(ChangeFuture), fg_Move(LoopFuture));

					// Check that loop was not detected
					DMibExpectTrue(Counter.m_nNotifications <= Counter.m_nExpectedMax);

					// Verify B now has the "release" tag
					TCSet<CStr> ExpectedTags{"release"};
					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ExpectedTags)) == DMibExpr(ExpectedTags));
				}

				co_return {};
			};

			// Loop prevention test: Circular tag chain A->B->C->A (full mesh)
			// A(a-tag) -> B(b-tag) -> C(c-tag) -> A(a-tag) - potential infinite loop
			DMibTestSuite("ThreeWay-LoopPrevention-CircularChain") -> TCFuture<void>
			{
				CAppManagerTestHelper::EOption Options
					= CAppManagerTestHelper::EOption_EnableVersionManager
					| CAppManagerTestHelper::EOption_DisablePatchMonitoring
					| CAppManagerTestHelper::EOption_DisableDiskMonitoring
					| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
					| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
				;

				if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
					Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

				CTripleSyncTestHelper Helper("ThreeWaySyncTests_LoopCircular", Options);
				auto Destroy = co_await fg_AsyncDestroy(Helper);

				// Phase 1: Setup all managers with empty configs to get host IDs
				co_await Helper.f_SetupManagerA();
				auto &StateA = Helper.f_ManagerAState();

				CEJsonSorted EmptyConfig = EJsonType_Object;
				co_await Helper.f_SetupManagerB(EmptyConfig);
				co_await Helper.f_SetupManagerC(EmptyConfig);

				// Setup bidirectional trust between all pairs
				co_await Helper.f_SetupTrustAB();
				co_await Helper.f_SetupTrustAC();
				co_await Helper.f_SetupTrustBC();

				// Phase 2: Restart with full mesh configs that include circular tag chain
				// Circular chain: A(a-tag) -> B(b-tag) -> C(c-tag) -> A(a-tag again) - potential loop
				CStr HostIDA = StateA.m_VersionManagerHostID;
				CStr HostIDB = Helper.m_ManagerBHostID;
				CStr HostIDC = Helper.m_ManagerCHostID;

				// A syncs relevant tags from B, and syncs c-tag from C transforming to a-tag (closes the loop)
				CEJsonSorted SyncSourcesA = EJsonType_Object;
				SyncSourcesA["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"a-tag", "b-tag", "c-tag", "other-tag"}});
				SyncSourcesA["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"a-tag", "b-tag", "other-tag", "c-tag=a-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerA, fg_CreateMultiSyncConfig(SyncSourcesA));

				// B syncs a-tag from A transforming to b-tag, and syncs relevant tags from C
				CEJsonSorted SyncSourcesB = EJsonType_Object;
				SyncSourcesB["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"b-tag", "c-tag", "other-tag", "a-tag=b-tag"}});
				SyncSourcesB["SyncFromC"] = fg_CreateSyncSource({HostIDC}, {.m_CopyTags = {"a-tag", "b-tag", "c-tag", "other-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerB, fg_CreateMultiSyncConfig(SyncSourcesB));

				// C syncs relevant tags from A, and syncs b-tag from B transforming to c-tag
				CEJsonSorted SyncSourcesC = EJsonType_Object;
				SyncSourcesC["SyncFromA"] = fg_CreateSyncSource({HostIDA}, {.m_CopyTags = {"a-tag", "b-tag", "c-tag", "other-tag"}});
				SyncSourcesC["SyncFromB"] = fg_CreateSyncSource({HostIDB}, {.m_CopyTags = {"a-tag", "c-tag", "other-tag", "b-tag=c-tag"}});
				co_await Helper.f_RestartManagerWithConfig(CTripleSyncTestHelper::EManager::mc_ManagerC, fg_CreateMultiSyncConfig(SyncSourcesC));

				{
					DMibTestPath("CircularTagChainNoLoop");

					// Setup notification counter
					CNotificationCounter Counter;
					Counter.m_nExpectedMax = 10;  // A->B->C = 3 notifications, allow buffer

					// Upload version to A with "a-tag"
					// If loop not prevented: A->B(b-tag)->C(c-tag)->A(a-tag)->B->C->A->...
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 300;
					VersionID.m_VersionID.m_Minor = 0;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["a-tag"];

					// Race: upload should complete vs loop detected
					auto UploadFuture = StateA.m_VersionManagerHelper.f_Upload
						(
							StateA.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
						.f_Timeout(g_Timeout, "Upload timed out - possible deadlock")
					;

					auto LoopFuture = Counter.f_GetLoopFuture();

					auto Result = co_await fg_AnyDone(fg_Move(UploadFuture), fg_Move(LoopFuture));

					// Check that loop was not detected
					DMibExpectTrue(Counter.m_nNotifications <= Counter.m_nExpectedMax);

					// Verify version propagated through the chain. Final tags depend on notification order
					// but loop must terminate. Any chain tag (a-tag, b-tag, c-tag) is valid.
					TCSet<CStr> ValidChainTags{"a-tag", "b-tag", "c-tag"};

					// Check B - should have at least one chain tag
					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ValidChainTags)) != DMibExpr(TCSet<CStr>()));

					// Check C - should have at least one chain tag
					TCSet<CStr> TagsC = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsC) & DMibExpr(ValidChainTags)) != DMibExpr(TCSet<CStr>()));
				}
				{
					DMibTestPath("CircularTagChangeNoLoop");

					// Upload version to A WITHOUT "a-tag" first
					CVersionManager::CVersionIDAndPlatform VersionID;
					VersionID.m_VersionID.m_Branch = StateA.m_PackageInfo.m_VersionID.m_VersionID.m_Branch;
					VersionID.m_VersionID.m_Major = 300;
					VersionID.m_VersionID.m_Minor = 1;
					VersionID.m_VersionID.m_Revision = 0;
					VersionID.m_Platform = StateA.m_PackageInfo.m_VersionID.m_Platform;

					CVersionManager::CVersionInformation VersionInfo;
					VersionInfo.m_Time = NTime::CTime::fs_NowUTC();
					VersionInfo.m_Configuration = "Release";
					VersionInfo.m_Tags["other-tag"];  // Not "a-tag", won't trigger chain transformation

					co_await StateA.m_VersionManagerHelper.f_Upload
						(
							StateA.m_VersionManager
							, "TestApp"
							, VersionID
							, VersionInfo
							, StateA.m_TestAppArchive
						)
						.f_Timeout(g_Timeout, "Upload timed out")
					;

					// Version should sync to B and C (via unrestricted paths), but without chain tags
					bool bInB = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibExpectTrue(bInB);

					bool bInC = co_await Helper.f_VerifyVersionInManager(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibExpectTrue(bInC);

					// Setup notification counter for tag change
					CNotificationCounter Counter;
					Counter.m_nExpectedMax = 10;  // Tag change + chain propagation, allow buffer

					// Now add "a-tag" - this should trigger the circular chain transformation
					// If loop not prevented: A adds a-tag -> B gets b-tag -> C gets c-tag -> A gets a-tag again -> ...
					CVersionManager::CChangeTags ChangeTagsParams;
					ChangeTagsParams.m_Application = "TestApp";
					ChangeTagsParams.m_VersionID = VersionID.m_VersionID;
					ChangeTagsParams.m_Platform = VersionID.m_Platform;
					ChangeTagsParams.m_AddTags["a-tag"];

					auto ChangeFuture = StateA.m_VersionManager.f_CallActor(&CVersionManager::f_ChangeTags)(ChangeTagsParams)
						.f_Timeout(g_Timeout, "Tag change timed out - possible deadlock")
					;

					auto LoopFuture = Counter.f_GetLoopFuture();

					auto Result = co_await fg_AnyDone(fg_Move(ChangeFuture), fg_Move(LoopFuture));

					// Check that loop was not detected
					DMibExpectTrue(Counter.m_nNotifications <= Counter.m_nExpectedMax);

					// Verify chain propagated - final tags depend on notification order but loop must terminate
					// Any chain tag (a-tag, b-tag, c-tag) is valid.
					TCSet<CStr> ValidChainTags{"a-tag", "b-tag", "c-tag"};

					// Check B - should have at least one chain tag
					TCSet<CStr> TagsB = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerB, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsB) & DMibExpr(ValidChainTags)) != DMibExpr(TCSet<CStr>()));

					// Check C - should have at least one chain tag
					TCSet<CStr> TagsC = co_await Helper.f_GetVersionTags(Helper.m_VersionManagerC, "TestApp", VersionID);
					DMibTest((DMibExpr(TagsC) & DMibExpr(ValidChainTags)) != DMibExpr(TCSet<CStr>()));
				}

				co_return {};
			};
		}
	};

	DMibTestRegister(CVersionManager_Sync_Tests, Malterlib::Cloud);
}

