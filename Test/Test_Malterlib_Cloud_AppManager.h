// Copyright © 2020 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#pragma once

#include <Mib/Core/Core>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Process/ProcessLaunchActor>
#include <Mib/Concurrency/DistributedAppInterfaceLaunch>
#include <Mib/Concurrency/DistributedTrustTestHelpers>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/DistributedAppInterface>
#include <Mib/Concurrency/DistributedActorTrustManagerProxy>
#include <Mib/Concurrency/DistributedAppTestHelpers>
#include <Mib/Cloud/AppManager>
#include <Mib/Cloud/CloudManager>
#include <Mib/Cloud/VersionManager>
#include <Mib/Cryptography/RandomID>
#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cloud/App/AppManager>
#include <Mib/Cloud/App/CloudManager>
#include <Mib/Cloud/App/VersionManager>

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
using namespace NMib::NTest;

namespace NMib::NCloud
{
	struct CAppManagerTestHelper
	{
		enum EOption
		{
			EOption_None = 0
			, EOption_EnableLogging = DMibBit(0)
			, EOption_EnableOtherOutput = DMibBit(1)
			, EOption_EnableVersionManager = DMibBit(2)
			, EOption_LaunchTestAppInApp = DMibBit(3)
		};

		struct CAppManagerInfo
		{
			CStr const &f_GetHostID() const;

			TCSharedPointer<TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface>> m_pTrustInterface;
			TCDistributedActor<CAppManagerInterface> m_Interface;
			CDistributedActorTrustManager_Address m_Address;
			TCOptional<CDistributedApp_LaunchInfo> m_Launch;
			CStr m_RootDirectory;
			CStr m_Name;
		};

		CAppManagerTestHelper(CStr const &_RootDirectory, EOption _Options, fp64 _Timeout);
		~CAppManagerTestHelper();

		template <typename tf_CHostID, typename tf_CPermissions>
		static auto fs_Permissions(tf_CHostID &&_HostID, tf_CPermissions &&_Permissions)
		{
			return CDistributedActorTrustManagerInterface::CAddPermissions{{_HostID, ""}, _Permissions, mc_WaitForSubscriptions};
		}

		template <typename tf_CNamespace, typename tf_CHosts>
		static auto fs_NamespaceHosts(tf_CNamespace &&_Namespace, tf_CHosts &&_Hosts)
		{
			return CDistributedActorTrustManagerInterface::CChangeNamespaceHosts{_Namespace, _Hosts, mc_WaitForSubscriptions};
		}

		void f_SetupTrust();
		void f_InstallTestApp(CStr const &_Name = "TestApp", CStr const &_Tag = "TestTag", CStr const &_Group = "TestGroup", CStr const &_VersionManagerApplication = "TestApp");
		void f_CheckCloudManager(mint _Sequence);
		void f_Setup(mint _nAppManagers);

		static auto constexpr mc_WaitForSubscriptions = EDistributedActorTrustManagerOrderingFlag_WaitForSubscriptions;

		TCSharedPointer<CDefaultRunLoop> m_pRunLoop = fg_Construct();
		TCActor<CDispatchingActor> m_HelperActor{fg_Construct(), m_pRunLoop->f_Dispatcher()};
		CCurrentlyProcessingActorScope m_CurrentActor{m_HelperActor};

		CStr m_ProgramDirectory = CFile::fs_GetProgramDirectory();
		CStr m_RootDirectory;

		TCMap<CStr, CPermissionRequirements> m_CloudManagerPermissionsForTest = {{"CloudManager/ReadAll"}};
		TCMap<CStr, CPermissionRequirements> m_VersionManagerPermissionsForTest = {{"Application/WriteAll"}, {"Application/ReadAll"}, {"Application/TagAll"}};

		CStr m_TestHostID;
		CDistributedActorTrustManager_Address m_ServerAddress;

		CTrustManagerTestHelper m_TrustManagerState;
		TCActor<CDistributedActorTrustManager> m_TrustManager;
		TCOptional<CTrustedSubscriptionTestHelper> m_Subscriptions;
		TCActor<CDistributedApp_LaunchHelper> m_LaunchHelper;

		CVersionManagerHelper m_VersionManagerHelper{m_ProgramDirectory / "TestApps/VersionManager"};
		CVersionManagerHelper::CPackageInfo m_PackageInfo;

		CStr m_CloudClientHostID;
		CStr m_TestAppArchive;

		CStr m_VersionManagerDirectory;
		TCOptional<CDistributedApp_LaunchInfo> m_VersionManagerLaunch;
		CDistributedActorTrustManager_Address m_VersionManagerServerAddress;
		CStr m_VersionManagerHostID;
		TCDistributedActor<CVersionManager> m_VersionManager;

		CStr m_CloudManagerDirectory;
		TCOptional<CDistributedApp_LaunchInfo> m_CloudManagerLaunch;
		CStr m_CloudClientDirectory;
		CStr m_CloudManagerHostID;
		TCDistributedActor<CCloudManager> m_CloudManager;
		CDistributedActorTrustManager_Address m_CloudManagerServerAddress;

		TCMap<CStr, CAppManagerInfo> m_AppManagerInfos;
		TCSet<CStr> m_AppManagerHosts;
		TCVector<COnScopeExitShared> m_InProcessLaunchScopes;

		mint m_nAppManagers = 0;
		fp64 m_Timeout = 60.0;
		EOption m_Options = EOption_None;
	};
}
