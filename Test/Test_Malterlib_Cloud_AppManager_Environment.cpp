// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Test/Exception>

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
#else
#	include <Mib/Core/PlatformSpecific/PosixUser>
#	include <unistd.h>
#endif

namespace
{
	fp64 g_Timeout = 120.0 * NMib::NTest::gc_TimeoutMultiplier;
}

struct CAppManager_Environment_Tests : public NMib::NTest::CTest
{
	void f_DoTests();
};

void CAppManager_Environment_Tests::f_DoTests()
{
	DMibTestSuite("General") -> TCFuture<void>
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

		CAppManagerTestHelper AppManagerTestHelper("AppManagerEnvironmentTests", Options, g_Timeout);

		auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

		co_await AppManagerTestHelper.f_Setup(1);

		auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
		auto &Interface = AppManagerInfo.m_Interface;

		auto fGetEnvironments = [&]() -> TCUnsafeFuture<TCMap<CStr, CAppManagerInterface::CEnvironmentInfo>>
			{
				co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetEnvironments)().f_Timeout(g_Timeout, "Timed out enumerating environments");
			}
		;

		{
			DMibTestPath("Initial");

			auto Environments = co_await fGetEnvironments();
			DMibExpect(Environments.f_GetLen(), ==, 0u);
		}

		{
			DMibTestPath("Add Local");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("TestEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");
		}

		{
			DMibTestPath("List Added");

			auto Environments = co_await fGetEnvironments();
			DMibExpect(Environments.f_GetLen(), ==, 1u);

			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Type, ==, CAppManagerInterface::EEnvironmentType_Local);
			DMibExpect(pEnvironment->m_bAutoStart, ==, false);
			DMibExpectTrue(pEnvironment->m_Applications.f_IsEmpty());
		}

		{
			DMibTestPath("Duplicate Add");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("TestEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Name Differing In Case");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("testenv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("only in case") >= 0);
		}

		{
			DMibTestPath("Container Without Image");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ContainerEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Parent Application Missing");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			Settings.m_ParentApplication = CStr("NoSuchApp");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ConfinedEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Add Container");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerImage = CStr("ubuntu:24.04");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ContainerEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");
		}

		{
			DMibTestPath("Change Settings");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_ContainerNetwork = CStr("bridge");
			Settings.m_MemoryLimitMB = 512;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)("ContainerEnv", Settings).f_Timeout(g_Timeout, "Timed out changing environment settings");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("ContainerEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_ContainerNetwork, ==, "bridge");
			DMibExpect(pEnvironment->m_MemoryLimitMB, ==, 512u);
			DMibExpect(pEnvironment->m_ContainerImage, ==, "ubuntu:24.04");
			DMibExpectTrue(pEnvironment->m_AgentApplication.f_StartsWith("SelfUpdate.Linux-"));
		}

		// The test client has access to all environments but not to the host
		{
			DMibTestPath("Host Access Denied");

			CAppManagerInterface::CEnvironmentSettings NetworkSettings;
			NetworkSettings.m_ContainerNetwork = CStr("host");

			auto NetworkResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ContainerEnv", NetworkSettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(NetworkResult);
			if (!NetworkResult)
				DMibExpectTrue(NetworkResult.f_GetExceptionStr().f_Find("Access denied") >= 0);

			CAppManagerInterface::CEnvironmentSettings ArgumentSettings;
			ArgumentSettings.m_ContainerExtraArguments = TCVector<CStr>{"--privileged"};

			auto ArgumentResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ContainerEnv", ArgumentSettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(ArgumentResult);

			CAppManagerInterface::CEnvironmentSettings JoinSettings;
			JoinSettings.m_ContainerNetwork = CStr("container:OtherContainer");

			auto JoinResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ContainerEnv", JoinSettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(JoinResult);
			if (!JoinResult)
				DMibExpectTrue(JoinResult.f_GetExceptionStr().f_Find("Access denied") >= 0);

			// The agent of a Local environment installs the dependencies on the host
			CAppManagerInterface::CEnvironmentSettings DependencySettings;
			DependencySettings.m_OSDependencies = TCVector<CStr>{"htop"};

			auto DependencyResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("TestEnv", DependencySettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(DependencyResult);
			if (!DependencyResult)
				DMibExpectTrue(DependencyResult.f_GetExceptionStr().f_Find("Access denied") >= 0);

			CAppManagerInterface::CEnvironmentSettings AddSettings;
			AddSettings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			AddSettings.m_ContainerImage = CStr("ubuntu:24.04");
			AddSettings.m_ContainerExtraMounts = TCMap<CStr, CStr>{{"/", "/Host"}};
			AddSettings.m_AutoStart = false;

			auto AddResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("MountEnv", AddSettings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(AddResult);

			auto Environments = co_await fGetEnvironments();
			DMibExpectTrue(Environments.f_FindEqual("MountEnv") == nullptr);
			auto pEnvironment = Environments.f_FindEqual("ContainerEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			if (pEnvironment)
			{
				DMibExpect(pEnvironment->m_ContainerNetwork, ==, "bridge");
				DMibExpectTrue(pEnvironment->m_ContainerExtraArguments.f_IsEmpty());
			}
		}

		{
			DMibTestPath("Invalid Change Settings");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_ContainerImage = CStr("");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ContainerEnv", Settings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(Result);
		}

#ifdef DPlatformFamily_macOS
		{
			DMibTestPath("Fractional CPUs With Apple Container");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerRuntime = CStr("AppleContainer");
			Settings.m_ContainerImage = CStr("ubuntu:24.04");
			Settings.m_CPULimit = 1.5;
			Settings.m_AutoStart = false;

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("FractionalEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("whole number of CPUs") >= 0);
		}

		{
			DMibTestPath("Parent Application With Apple Container");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerImage = CStr("ubuntu:24.04");
			Settings.m_ParentApplication = CStr("EnvApp");
			Settings.m_AutoStart = false;

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ParentAppleEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("need the Colima container runtime") >= 0);
		}

		// The Colima virtual machine can keep the backing image of the data mounted, which only a removal through Colima unmounts
		{
			DMibTestPath("Colima Data Image Keeps Runtime");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerRuntime = CStr("Colima");
			Settings.m_ContainerImage = CStr("ubuntu:24.04");
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ColimaDataEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			co_await
				(
					g_Dispatch / [DataDirectory = AppManagerInfo.m_RootDirectory / ".Environment/ColimaDataEnv/Data"]
					{
						CFile::fs_CreateDirectory(DataDirectory);
						CFile::fs_WriteStringToFile(DataDirectory / "DataDisk.img", "");
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating data image")
			;

			CAppManagerInterface::CEnvironmentSettings ChangeSettings;
			ChangeSettings.m_ContainerRuntime = CStr("AppleContainer");

			auto ChangeResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ColimaDataEnv", ChangeSettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(ChangeResult);
			if (!ChangeResult)
				DMibExpectTrue(ChangeResult.f_GetExceptionStr().f_Find("The data volume of environment 'ColimaDataEnv' exists") >= 0);

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("ColimaDataEnv").f_Timeout(g_Timeout, "Timed out removing environment");
		}
#endif

		{
			DMibTestPath("Unknown Container Runtime");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerRuntime = CStr("/bin/sh");
			Settings.m_ContainerImage = CStr("ubuntu:24.04");
			Settings.m_AutoStart = false;

			auto AddResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("RuntimeEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(AddResult);
			if (!AddResult)
				DMibExpectTrue(AddResult.f_GetExceptionStr().f_Find("is not a supported container runtime") >= 0);

			CAppManagerInterface::CEnvironmentSettings ChangeSettings;
			ChangeSettings.m_ContainerRuntime = CStr("/bin/sh");

			auto ChangeResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)
				("ContainerEnv", ChangeSettings)
				.f_Timeout(g_Timeout, "Timed out changing environment settings")
				.f_Wrap()
			;
			DMibExpectFalse(ChangeResult);
			if (!ChangeResult)
				DMibExpectTrue(ChangeResult.f_GetExceptionStr().f_Find("is not a supported container runtime") >= 0);
		}

		{
			DMibTestPath("Referencing Application");

			CAppManagerInterface::CApplicationAdd Add;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("TestEnv");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Add)("EnvApp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Applications.f_GetLen(), ==, 1u);
			DMibExpectTrue(pEnvironment->m_Applications.f_FindEqual("EnvApp") != nullptr);
		}

		// The installed files stay where they are, and a container keeps application files only in its own storage
		{
			DMibTestPath("Move Into Container Rejected");

			CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("ContainerEnv");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)
				("EnvApp", ChangeSettings, Settings)
				.f_Timeout(g_Timeout, "Timed out changing application settings")
				.f_Wrap()
			;
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("cannot move into it from host storage") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			auto pEnvApp = Installed.f_FindEqual("EnvApp");
			DMibExpectTrue(pEnvApp != nullptr);
			if (pEnvApp)
				DMibExpect(pEnvApp->m_LaunchEnvironment, ==, "TestEnv");
		}

		{
			DMibTestPath("Remove Referenced");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv").f_Timeout(g_Timeout, "Timed out removing environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		// Without an agent application the agent is a copy of the running executable, which is the test runner here and
		// not an AppManager, so the start fails
		{
			DMibTestPath("Start Without AppManager Agent");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("TestEnv").f_Timeout(g_Timeout, "Timed out starting environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Remove After Last Application");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Remove)("EnvApp").f_Timeout(g_Timeout, "Timed out removing application");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv").f_Timeout(g_Timeout, "Timed out removing environment");

			auto Environments = co_await fGetEnvironments();
			DMibExpect(Environments.f_GetLen(), ==, 1u);
			DMibExpectTrue(Environments.f_FindEqual("TestEnv") == nullptr);
		}

		{
			DMibTestPath("Remove Missing");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv").f_Timeout(g_Timeout, "Timed out removing environment").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Agent Package For Another Platform");

			CStr PackageDirectory = AppManagerTestHelper.f_RootDirectory() / "WrongPlatformAgent";

			co_await
				(
					g_Dispatch / [PackageDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "AppManager";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "Windows-x64";

						CFile::fs_CreateDirectory(PackageDirectory);
						CFile::fs_WriteStringToFile(PackageDirectory / "AppManagerVersionInfo.json", VersionInfo.f_ToString());
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent package")
			;

			auto Result = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", "--agent-platforms", "[\"Linux-arm64={}\"]"_f << PackageDirectory, PackageDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;

			DMibExpect(Result.m_ExitCode, !=, 0u);
			DMibExpectTrue(Result.f_GetCombinedOut().f_Find("for agent platform 'Linux-arm64' is for platform 'Windows-x64'") >= 0);

			CStr NoVersionInfoDirectory = AppManagerTestHelper.f_RootDirectory() / "NoVersionInfoAgent";
			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [NoVersionInfoDirectory]
					{
						CFile::fs_CreateDirectory(NoVersionInfoDirectory);
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent package")
			;

			auto NoVersionInfoResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", "--agent-platforms", "[\"Linux-arm64={}\"]"_f << NoVersionInfoDirectory, PackageDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;

			DMibExpect(NoVersionInfoResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(NoVersionInfoResult.f_GetCombinedOut().f_Find("for agent platform 'Linux-arm64' has no readable version info") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpectTrue(Installed.f_FindEqual("SelfUpdate.Linux-arm64") == nullptr);
			DMibExpectTrue(Installed.f_FindEqual("SelfUpdate.Windows-x64") == nullptr);
		}

		{
			DMibTestPath("Agent Update For Another Platform");

			CStr AgentDirectory = AppManagerTestHelper.f_RootDirectory() / "OtherPlatformAgent";
			CStr WrongPlatformDirectory = AppManagerTestHelper.f_RootDirectory() / "WrongPlatformAgent";

			co_await
				(
					g_Dispatch / [AgentDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "AppManager";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "Linux-riscv64";

						CFile::fs_CreateDirectory(AgentDirectory);
						CFile::fs_WriteStringToFile(AgentDirectory / "AppManagerVersionInfo.json", VersionInfo.f_ToString());
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent package")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", AgentDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			auto AddAgainResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", "--agent-platforms", "[\"Linux-riscv64={}\"]"_f << AgentDirectory, AgentDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;
			DMibExpect(AddAgainResult.m_ExitCode, ==, 0u);
			DMibExpectTrue(AddAgainResult.f_GetCombinedOut().f_Find("Application 'SelfUpdate.Linux-riscv64' is already installed") >= 0);

			auto UpdateResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-update-from-file", "--name", "SelfUpdate.Linux-riscv64", WrongPlatformDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out updating agent application")
			;
			DMibExpect(UpdateResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(UpdateResult.f_GetCombinedOut().f_Find("provides the agent for platform 'Linux-riscv64'") >= 0);

			auto VersionManagerUpdateResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-update", "--name", "SelfUpdate.Linux-riscv64", "--platform", "Linux-x64"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out updating agent application")
			;
			DMibExpect(VersionManagerUpdateResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(VersionManagerUpdateResult.f_GetCombinedOut().f_Find("provides the agent for platform 'Linux-riscv64'") >= 0);

			CStr OtherApplicationDirectory = AppManagerTestHelper.f_RootDirectory() / "OtherApplicationAgent";

			co_await
				(
					g_Dispatch / [OtherApplicationDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "OtherApplication";
						VersionInfo["Version"] = "master/2.0.0";
						VersionInfo["Platform"] = "Linux-riscv64";

						CFile::fs_CreateDirectory(OtherApplicationDirectory);
						CFile::fs_WriteStringToFile(OtherApplicationDirectory / "OtherApplicationVersionInfo.json", VersionInfo.f_ToString());
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating package")
			;

			auto OtherApplicationResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-update-from-file", "--name", "SelfUpdate.Linux-riscv64", OtherApplicationDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out updating agent application")
			;
			DMibExpect(OtherApplicationResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(OtherApplicationResult.f_GetCombinedOut().f_Find("The package is for application 'OtherApplication'") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			auto pAgent = Installed.f_FindEqual("SelfUpdate.Linux-riscv64");
			DMibAssertTrue(pAgent != nullptr);
			DMibExpect(pAgent->m_Version.m_Platform, ==, "Linux-riscv64");

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "SelfUpdate.Linux-riscv64"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing agent application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("Agent Application Name Taken");

			CStr OrdinaryDirectory = AppManagerTestHelper.f_RootDirectory() / "OrdinaryAgentName";
			CStr AgentDirectory = AppManagerTestHelper.f_RootDirectory() / "TakenNameAgent";

			co_await
				(
					g_Dispatch / [OrdinaryDirectory, AgentDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(OrdinaryDirectory);
						CFile::fs_WriteStringToFile(OrdinaryDirectory / "Data.txt", "Ordinary");

						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "AppManager";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "Linux-mips64";

						CFile::fs_CreateDirectory(AgentDirectory);
						CFile::fs_WriteStringToFile(AgentDirectory / "AppManagerVersionInfo.json", VersionInfo.f_ToString());
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating packages")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", OrdinaryDirectory, "--name", "SelfUpdate.Linux-mips64"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			auto EnableResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", "--agent-platforms", "[\"Linux-mips64={}\"]"_f << AgentDirectory, AgentDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;
			DMibExpect(EnableResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(EnableResult.f_GetCombinedOut().f_Find("isn't the agent distribution for platform 'Linux-mips64'") >= 0);

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "SelfUpdate.Linux-mips64"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);

			// An ordinary application with the name of the host installation doesn't enable self update. A removed application keeps its
			// directory, so the agent is for another platform
			CStr HostNameAgentDirectory = AppManagerTestHelper.f_RootDirectory() / "HostNameTakenAgent";
			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [HostNameAgentDirectory]
					{
						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "AppManager";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "Linux-ppc64le";

						CFile::fs_CreateDirectory(HostNameAgentDirectory);
						CFile::fs_WriteStringToFile(HostNameAgentDirectory / "AppManagerVersionInfo.json", VersionInfo.f_ToString());
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating package")
			;

			auto AddHostNameResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", OrdinaryDirectory, "--name", "SelfUpdate"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddHostNameResult.m_ExitCode, ==, 0u);

			auto EnableHostNameResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-enable-self-update", "--from-file", "--agent-platforms", "[\"Linux-ppc64le={}\"]"_f << HostNameAgentDirectory, OrdinaryDirectory}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out enabling self update")
			;
			DMibExpect(EnableHostNameResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(EnableHostNameResult.f_GetCombinedOut().f_Find("Application 'SelfUpdate' is installed, but isn't the self update source") >= 0);

			for (auto &Name : {CStr("SelfUpdate"), CStr("SelfUpdate.Linux-ppc64le")})
			{
				DMibTestPath(Name);

				auto RemoveHostNameResult = co_await AppManagerTestHelper.f_LaunchSimple
					(
						AppManagerInfo.m_RootDirectory / "AppManager"
						, {"--application-remove", Name}
						, AppManagerInfo.m_RootDirectory
					)
					.f_Timeout(g_Timeout, "Timed out removing application")
				;
				DMibExpect(RemoveHostNameResult.m_ExitCode, ==, 0u);
			}
		}

#ifndef DPlatformFamily_Windows
		{
			DMibTestPath("Linked Non-Executable On Host");

			CStr PackageDirectory = AppManagerTestHelper.f_RootDirectory() / "LinkedNonExecutable";

			co_await
				(
					g_Dispatch / [PackageDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(PackageDirectory);
						CFile::fs_WriteStringToFile(PackageDirectory / "Data.txt", "Not executable");
						CFile::fs_CreateSymbolicLink("Data.txt", PackageDirectory / "Executable", EFileAttrib_File, ESymbolicLinkFlag_None);
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating package")
			;

			auto Result = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", PackageDirectory, "--name", "LinkedNonExecutable", "--executable", "Executable"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;

			DMibExpect(Result.m_ExitCode, !=, 0u);
			DMibExpectTrue(Result.f_GetCombinedOut().f_Find("does not exist or does not have the executable flag set") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpectTrue(Installed.f_FindEqual("LinkedNonExecutable") == nullptr);

			// A Local environment uses host storage, so the link must resolve to an executable on the host as well
			CAppManagerInterface::CEnvironmentSettings LocalSettings;
			LocalSettings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			LocalSettings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("LinkedLocalEnv", LocalSettings).f_Timeout(g_Timeout, "Timed out adding environment");

			auto LocalResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", PackageDirectory, "--name", "LinkedNonExecutableLocal", "--executable", "Executable", "--launch-environment", "LinkedLocalEnv"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;

			DMibExpect(LocalResult.m_ExitCode, !=, 0u);
			DMibExpectTrue(LocalResult.f_GetCombinedOut().f_Find("does not exist or does not have the executable flag set") >= 0);

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("LinkedLocalEnv").f_Timeout(g_Timeout, "Timed out removing environment");
		}
#endif

		{
			DMibTestPath("OS Dependencies Without Executable");

			CStr PackageDirectory = AppManagerTestHelper.f_RootDirectory() / "DependenciesWithoutExecutable";

			// An invalid package name fails the installation before a package manager runs
			co_await
				(
					g_Dispatch / [PackageDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "DependenciesWithoutExecutable";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "TestPlatform";
						auto &Dependencies = VersionInfo["ExtraInfo"]["OSDependencies"];
						for (auto &Selector : {CStr("Linux"), CStr("macOS"), CStr("Windows")})
							Dependencies[Selector] = _["invalid;package"];

						CFile::fs_CreateDirectory(PackageDirectory);
						CFile::fs_WriteStringToFile(PackageDirectory / "DependenciesWithoutExecutableVersionInfo.json", VersionInfo.f_ToString());
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating package")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", PackageDirectory, "--name", "DependenciesWithoutExecutable"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			CStr Status;
			NTime::CStopwatch Stopwatch{true};
			while (true)
			{
				auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
				if (auto pApplication = Installed.f_FindEqual("DependenciesWithoutExecutable"))
					Status = pApplication->m_Status;

				if (Status == "No exe" || Status.f_Find("OS dependencies") >= 0 || Stopwatch.f_GetTime() > g_Timeout)
					break;

				co_await fg_Timeout(0.1);
			}

			DMibExpect(Status, ==, "No exe");

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "DependenciesWithoutExecutable"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("Settings From Version Info Without OS Dependencies");

			CStr PackagesDirectory = AppManagerTestHelper.f_RootDirectory() / "DroppedDependenciesPackages";

			co_await
				(
					g_Dispatch / [PackagesDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						for (auto &Version : {CStr("1.0.0"), CStr("1.0.1")})
						{
							CStr PackageDirectory = PackagesDirectory / Version;

							CEJsonSorted VersionInfo;
							VersionInfo["Application"] = "DroppedDependencies";
							VersionInfo["Version"] = "master/{}"_f << Version;
							VersionInfo["Platform"] = "TestPlatform";

							// The selector matches no OS, so nothing is installed
							if (Version == "1.0.0")
								VersionInfo["ExtraInfo"]["OSDependencies"]["NoSuchOS"] = _["curl"];

							CFile::fs_CreateDirectory(PackageDirectory);
							CFile::fs_WriteStringToFile(PackageDirectory / "DroppedDependenciesVersionInfo.json", VersionInfo.f_ToString());
						}
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating packages")
			;

			auto fLaunch = [&](TCVector<CStr> _Parameters) -> TCUnsafeFuture<uint32>
				{
					auto Result = co_await AppManagerTestHelper.f_LaunchSimple(AppManagerInfo.m_RootDirectory / "AppManager", fg_Move(_Parameters), AppManagerInfo.m_RootDirectory)
						.f_Timeout(g_Timeout, "Timed out running AppManager command")
					;
					co_return Result.m_ExitCode;
				}
			;

			auto fHasDependencies = [&]() -> TCUnsafeFuture<bool>
				{
					auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
					auto pApplication = Installed.f_FindEqual("DroppedDependencies");
					co_return pApplication && !pApplication->m_OSDependencies.f_IsEmpty();
				}
			;

			DMibAssert(co_await fLaunch({"--application-add", "--from-file", PackagesDirectory / "1.0.0", "--name", "DroppedDependencies"}), ==, 0u);
			bool bAddedHasDependencies = co_await fHasDependencies();
			DMibExpectTrue(bAddedHasDependencies);

			DMibAssert(co_await fLaunch({"--application-update-from-file", "--name", "DroppedDependencies", "--update-settings=false", PackagesDirectory / "1.0.1"}), ==, 0u);
			bool bUpdatedHasDependencies = co_await fHasDependencies();
			DMibExpectTrue(bUpdatedHasDependencies);

			DMibAssert(co_await fLaunch({"--application-change-settings", "--name", "DroppedDependencies", "--update-from-version-info"}), ==, 0u);
			bool bReappliedHasDependencies = co_await fHasDependencies();
			DMibExpectFalse(bReappliedHasDependencies);

			DMibExpect(co_await fLaunch({"--application-remove", "DroppedDependencies"}), ==, 0u);
		}

		{
			DMibTestPath("Add Without Settings From Version Info");

			// The version time of a directory package is the time of its newest file, as the upload records it
			CStr PackageDirectory = AppManagerTestHelper.f_RootDirectory() / "NoSettingsPackage";
			NTime::CTime PayloadTime = NTime::CTimeConvert::fs_FromUnixMilliseconds(1'700'000'000'123);

			auto BlockingActorCheckout = fg_BlockingActor();
			co_await
				(
					g_Dispatch(BlockingActorCheckout) / [PackageDirectory, PayloadTime]
					{
						CEJsonSorted VersionInfo;
						VersionInfo["Application"] = "NoSettings";
						VersionInfo["Version"] = "master/1.0.0";
						VersionInfo["Platform"] = "TestPlatform";

						CFile::fs_CreateDirectory(PackageDirectory / "Sub");
						CFile::fs_WriteStringToFile(PackageDirectory / "NoSettingsVersionInfo.json", VersionInfo.f_ToString());
						CFile::fs_WriteStringToFile(PackageDirectory / "Sub/Data.txt", "Data");
						CFile::fs_SetWriteTime(PackageDirectory / "NoSettingsVersionInfo.json", PayloadTime - NTime::CTimeSpanConvert::fs_CreateSpanFromSeconds(60.0));
						CFile::fs_SetWriteTime(PackageDirectory / "Sub/Data.txt", PayloadTime);
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating package")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", PackageDirectory, "--name", "NoSettings", "--settings-from-version-info=false"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			auto pApplication = Installed.f_FindEqual("NoSettings");
			DMibAssertTrue(pApplication != nullptr);
			DMibExpect(pApplication->m_VersionManagerApplication, ==, "");
			DMibExpect(pApplication->m_VersionInfo.m_Time, ==, PayloadTime);

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "NoSettings"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("Host Update Deletes Files Of Previous Update");

			CStr PackagesDirectory = AppManagerTestHelper.f_RootDirectory() / "UpdatedFilesPackages";
			CStr ApplicationDirectory = AppManagerInfo.m_RootDirectory / "App/UpdatedFiles";

			co_await
				(
					g_Dispatch / [PackagesDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						for (auto &Version : {CStr("1.0.0"), CStr("1.0.1"), CStr("1.0.2")})
						{
							CStr PackageDirectory = PackagesDirectory / Version;

							CEJsonSorted VersionInfo;
							VersionInfo["Application"] = "UpdatedFiles";
							VersionInfo["Version"] = "master/{}"_f << Version;
							VersionInfo["Platform"] = "TestPlatform";

							// The selector matches no OS, so nothing is installed
							if (Version == "1.0.0")
								VersionInfo["ExtraInfo"]["OSDependencies"]["NoSuchOS"] = _["curl"];

							CFile::fs_CreateDirectory(PackageDirectory);
							CFile::fs_WriteStringToFile(PackageDirectory / "UpdatedFilesVersionInfo.json", VersionInfo.f_ToString());
							CFile::fs_WriteStringToFile(PackageDirectory / "Data.txt", Version);
						}

						CFile::fs_WriteStringToFile(PackagesDirectory / "1.0.1/Extra.txt", "Only in 1.0.1");
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating packages")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", PackagesDirectory / "1.0.0", "--name", "UpdatedFiles"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			for (auto &Version : {CStr("1.0.1"), CStr("1.0.2")})
			{
				DMibTestPath(Version);

				auto UpdateResult = co_await AppManagerTestHelper.f_LaunchSimple
					(
						AppManagerInfo.m_RootDirectory / "AppManager"
						, {"--application-update-from-file", "--name", "UpdatedFiles", PackagesDirectory / Version}
						, AppManagerInfo.m_RootDirectory
					)
					.f_Timeout(g_Timeout, "Timed out updating application")
				;
				DMibAssert(UpdateResult.m_ExitCode, ==, 0u);
			}

			auto [Data, bExtraExists] = co_await
				(
					g_Dispatch / [ApplicationDirectory]()
					{
						return fg_Tuple(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Data.txt"), CFile::fs_FileExists(ApplicationDirectory / "Extra.txt"));
					}
				)
				.f_Timeout(g_Timeout, "Timed out reading application files")
			;
			DMibExpect(Data, ==, "1.0.2");
			DMibExpectFalse(bExtraExists);

			{
				DMibTestPath("Same Version Keeps Installed Time");

				auto InstalledBefore = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
				auto pBefore = InstalledBefore.f_FindEqual("UpdatedFiles");
				DMibAssertTrue(pBefore != nullptr);

				// A version without OS dependencies has none
				DMibExpectTrue(pBefore->m_OSDependencies.f_IsEmpty());
				NTime::CTime InstalledTime = pBefore->m_VersionInfo.m_Time;
				DMibAssertTrue(InstalledTime.f_IsValid());

				co_await
					(
						g_Dispatch / [VersionInfoFile = PackagesDirectory / "1.0.2/UpdatedFilesVersionInfo.json", InstalledTime]
						{
							CFile::fs_SetWriteTime(VersionInfoFile, InstalledTime + NTime::CTimeSpanConvert::fs_CreateSpanFromSeconds(60.0));

#ifndef DPlatformFamily_Windows
							// The target of a link can only exist where the package is installed
							CFile::fs_CreateSymbolicLink("/MalterlibTestMissingTarget", CFile::fs_GetPath(VersionInfoFile) / "MissingTarget", EFileAttrib_File, ESymbolicLinkFlag_None);
#endif
						}
					)
					.f_Timeout(g_Timeout, "Timed out changing package time")
				;

				auto UpdateResult = co_await AppManagerTestHelper.f_LaunchSimple
					(
						AppManagerInfo.m_RootDirectory / "AppManager"
						, {"--application-update-from-file", "--name", "UpdatedFiles", PackagesDirectory / "1.0.2"}
						, AppManagerInfo.m_RootDirectory
					)
					.f_Timeout(g_Timeout, "Timed out updating application")
				;
				DMibAssert(UpdateResult.m_ExitCode, ==, 0u);

				auto InstalledAfter = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
				auto pAfter = InstalledAfter.f_FindEqual("UpdatedFiles");
				DMibAssertTrue(pAfter != nullptr);
				DMibExpect(pAfter->m_VersionInfo.m_Time, ==, InstalledTime);
			}

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "UpdatedFiles"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("Retry Of Update Stopped After File Replacement");

			CStr TestDirectory = AppManagerTestHelper.f_RootDirectory() / "RetriedUpdate";
			CStr ScriptPath = TestDirectory / "PostUpdate.sh";
			CStr PreviousVersionsPath = TestDirectory / "PreviousVersions.txt";
			CStr ErrorScriptPath = TestDirectory / "OnError.sh";
			CStr ErrorScriptStartedPath = TestDirectory / "OnError.Started";
			CStr ErrorScriptReleasePath = TestDirectory / "OnError.Release";

			co_await
				(
					g_Dispatch / [TestDirectory, ScriptPath, PreviousVersionsPath, ErrorScriptPath, ErrorScriptStartedPath, ErrorScriptReleasePath]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						for (auto &Version : {CStr("1.0.0"), CStr("1.0.1")})
						{
							CStr PackageDirectory = TestDirectory / Version;

							CEJsonSorted VersionInfo;
							VersionInfo["Application"] = "RetriedUpdate";
							VersionInfo["Version"] = "master/{}"_f << Version;
							VersionInfo["Platform"] = "TestPlatform";

							CFile::fs_CreateDirectory(PackageDirectory);
							CFile::fs_WriteStringToFile(PackageDirectory / "RetriedUpdateVersionInfo.json", VersionInfo.f_ToString());
						}

						// Fails the first update that runs it, which has replaced the files
						CFile::fs_WriteStringToFile
							(
								ScriptPath
								, "#!/bin/bash\n"
								"echo \"$MalterlibCloud_PreviousVersionID\" >> '{0}'\n"
								"if [ \"$MalterlibCloud_VersionID\" == 'master/1.0.1' ] && [ ! -f '{0}.Failed' ]; then\n"
								"\ttouch '{0}.Failed'\n"
								"\texit 1\n"
								"fi\n"_f
								<< PreviousVersionsPath
							)
						;

						// Holds the failed update until the test has seen its failure
						CFile::fs_WriteStringToFile
							(
								ErrorScriptPath
								, "#!/bin/bash\n"
								"touch '{}'\n"
								"for i in $(seq 1 6000); do\n"
								"\tif [ -f '{}' ]; then\n"
								"\t\texit 0\n"
								"\tfi\n"
								"\tsleep 0.1\n"
								"done\n"_f
								<< ErrorScriptStartedPath
								<< ErrorScriptReleasePath
							)
						;
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating packages")
			;

			TCSharedPointer<CStr> pNotifiedFailedVersion = fg_Construct();
			TCSharedPointer<TCOptional<bool>> pAddNotifiedFailure = fg_Construct();
			auto ChangeSubscription = co_await Interface.f_CallActor(&CAppManagerInterface::f_SubscribeChangeNotifications)
				(
					CAppManagerInterface::CSubscribeChangeNotifications
					{
						.m_fOnNotification = g_ActorFunctor / [pNotifiedFailedVersion, pAddNotifiedFailure](CAppManagerInterface::COnChangeNotificationParams _Params) -> TCFuture<void>
						{
							for (auto &Change : _Params.m_Changes)
							{
								if (Change.m_Application != "RetriedUpdate" || !Change.m_Change.f_IsOfType<CAppManagerInterface::CApplicationChange_AddOrChangeInfo>())
									continue;

								auto &Info = Change.m_Change.f_GetAsType<CAppManagerInterface::CApplicationChange_AddOrChangeInfo>().m_Info;
								*pNotifiedFailedVersion = CStr::fs_ToStr(Info.m_FailedVersion.m_VersionID);
								if (!*pAddNotifiedFailure)
									*pAddNotifiedFailure = Info.m_FailedVersion.f_IsValid();
							}

							co_return {};
						}
						, .m_bWaitForNotification = false
					}
				)
				.f_Timeout(g_Timeout, "Timed out subscribing to application changes")
			;

			auto AddResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					,
					{
						"--application-add"
						, "--from-file"
						, TestDirectory / "1.0.0"
						, "--name"
						, "RetriedUpdate"
						, "--update-script-post-update"
						, ScriptPath
						, "--update-script-on-error"
						, ErrorScriptPath
					}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding application")
			;
			DMibAssert(AddResult.m_ExitCode, ==, 0u);

			{
				DMibTestPath("Add Notifies No Failure");

				NTime::CStopwatch Stopwatch{true};
				while (!*pAddNotifiedFailure && Stopwatch.f_GetTime() < g_Timeout)
					co_await fg_Timeout(0.1);

				DMibAssertTrue((bool)*pAddNotifiedFailure);
				DMibExpectFalse(**pAddNotifiedFailure);
			}

			auto fUpdate = [&]() -> TCUnsafeFuture<uint32>
				{
					auto Result = co_await AppManagerTestHelper.f_LaunchSimple
						(
							AppManagerInfo.m_RootDirectory / "AppManager"
							, {"--application-update-from-file", "--name", "RetriedUpdate", TestDirectory / "1.0.1"}
							, AppManagerInfo.m_RootDirectory
						)
						.f_Timeout(g_Timeout, "Timed out updating application")
					;
					co_return Result.m_ExitCode;
				}
			;

			auto StoppedUpdate = fUpdate();

			{
				DMibTestPath("Failure Shown While The Error Script Runs");

				NTime::CStopwatch Stopwatch{true};
				bool bErrorScriptStarted = false;
				while (!bErrorScriptStarted && Stopwatch.f_GetTime() < g_Timeout)
				{
					co_await fg_Timeout(0.1);
					bErrorScriptStarted = co_await
						(
							g_Dispatch / [ErrorScriptStartedPath]()
							{
								return CFile::fs_FileExists(ErrorScriptStartedPath);
							}
						)
					;
				}
				DMibAssertTrue(bErrorScriptStarted);

				// The failure is recorded beside the error script, which holds the update until it is released below
				CStr FailedVersion;
				Stopwatch.f_Start();
				while (FailedVersion != "master/1.0.1" && Stopwatch.f_GetTime() < 30.0)
				{
					auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
					if (auto pInstalled = Installed.f_FindEqual("RetriedUpdate"))
						FailedVersion = CStr::fs_ToStr(pInstalled->m_FailedVersion.m_VersionID);

					if (FailedVersion != "master/1.0.1")
						co_await fg_Timeout(0.1);
				}
				DMibExpect(FailedVersion, ==, "master/1.0.1");
			}

			co_await
				(
					g_Dispatch / [ErrorScriptReleasePath]()
					{
						CFile::fs_WriteStringToFile(ErrorScriptReleasePath, "", false);
					}
				)
				.f_Timeout(g_Timeout, "Timed out releasing the error script")
			;

			uint32 StoppedUpdateExitCode = co_await fg_Move(StoppedUpdate);
			DMibExpect(StoppedUpdateExitCode, !=, 0u);

			{
				auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
				auto pInstalled = Installed.f_FindEqual("RetriedUpdate");
				DMibAssertTrue(pInstalled != nullptr);
				DMibExpect(CStr::fs_ToStr(pInstalled->m_FailedVersion.m_VersionID), ==, "master/1.0.1");
			}

			{
				DMibTestPath("Change Notification Of Failure");

				// The failure is notified once the update no longer runs
				NTime::CStopwatch Stopwatch{true};
				while (*pNotifiedFailedVersion != "master/1.0.1" && Stopwatch.f_GetTime() < g_Timeout)
					co_await fg_Timeout(0.1);

				DMibExpect(*pNotifiedFailedVersion, ==, "master/1.0.1");
			}

			co_await ChangeSubscription->f_Destroy();

			uint32 RetriedUpdateExitCode = co_await fUpdate();
			DMibExpect(RetriedUpdateExitCode, ==, 0u);

			CStr PreviousVersions = co_await
				(
					g_Dispatch / [PreviousVersionsPath]()
					{
						return CFile::fs_ReadStringFromFile(PreviousVersionsPath);
					}
				)
				.f_Timeout(g_Timeout, "Timed out reading previous versions")
			;
			DMibExpect(PreviousVersions, ==, "master/1.0.0\nmaster/1.0.0\n");

			auto RemoveResult = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "RetriedUpdate"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;
			DMibExpect(RemoveResult.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("VM Image Differing In Case");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_VM;
			Settings.m_VMImage = CStr("SharedImage");
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("VMEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			Settings.m_VMImage = CStr("sharedimage");
			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("VMEnvCase", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("is used by environment 'VMEnv'") >= 0);

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("VMEnv").f_Timeout(g_Timeout, "Timed out removing environment");
		}

		// Deleting the files of an application in environment-managed storage needs the environment running, which this one cannot
		{
			DMibTestPath("Application Name Differing In Case In Environment Storage");

			CAppManagerInterface::CEnvironmentSettings EnvironmentSettings;
			EnvironmentSettings.m_Type = CAppManagerInterface::EEnvironmentType_VM;
			EnvironmentSettings.m_VMImage = CStr("CaseImage");
			EnvironmentSettings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("CaseVMEnv", EnvironmentSettings).f_Timeout(g_Timeout, "Timed out adding environment");

			CAppManagerInterface::CApplicationAdd Add;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("CaseVMEnv");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Add)("CaseApp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_Add)("caseapp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application").f_Wrap();
			DMibExpectFalse(Result);
			if (!Result)
				DMibExpectTrue(Result.f_GetExceptionStr().f_Find("would share the environment storage of application 'CaseApp'") >= 0);
		}

		{
			DMibTestPath("Remove Deleting Application Data");

			auto Result = co_await AppManagerTestHelper.f_LaunchSimple
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-remove", "--delete-application-data", "CaseApp"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;

			DMibExpect(Result.m_ExitCode, !=, 0u);
			DMibExpectTrue(Result.f_GetCombinedOut().f_Find("Failed to remove the application storage in the environment") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpectTrue(Installed.f_FindEqual("CaseApp") != nullptr);
		}

		{
			DMibTestPath("Remove Keeping Application Data");

			CStr Output = co_await AppManagerTestHelper.f_LaunchTool(AppManagerInfo.m_RootDirectory / "AppManager", {"--application-remove", "CaseApp"}, AppManagerInfo.m_RootDirectory)
				.f_Timeout(g_Timeout, "Timed out removing application")
			;

			DMibExpectTrue(Output.f_Find("Kept the files of application 'CaseApp' in environment 'CaseVMEnv'") >= 0);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpectTrue(Installed.f_FindEqual("CaseApp") == nullptr);
		}

		{
			DMibTestPath("Remove With Settings Keeping Application Data");

			CAppManagerInterface::CApplicationAdd Add;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("CaseVMEnv");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Add)("KeptApp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_RemoveWithSettings)
				("KeptApp", CAppManagerInterface::CApplicationRemoveSettings())
				.f_Timeout(g_Timeout, "Timed out removing application")
			;

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpectTrue(Installed.f_FindEqual("KeptApp") == nullptr);
		}

		{
			DMibTestPath("Concurrent Adds Sharing Environment Storage");

			CAppManagerInterface::CApplicationAdd Add;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("CaseVMEnv");

			auto [SameNameFirst, SameNameSecond, OtherCase] = co_await
				(
					Interface.f_CallActor(&CAppManagerInterface::f_Add)("RaceApp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application")
					+ Interface.f_CallActor(&CAppManagerInterface::f_Add)("RaceApp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application")
					+ Interface.f_CallActor(&CAppManagerInterface::f_Add)("raceapp", Add, Settings).f_Timeout(g_Timeout, "Timed out adding application")
				)
				.f_Wrap()
			;

			umint nAdded = 0;
			for (auto *pResult : {&SameNameFirst, &SameNameSecond, &OtherCase})
			{
				if (*pResult)
					++nAdded;
			}

			DMibExpect(nAdded, ==, 1u);

			auto Installed = co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			DMibExpect(umint(Installed.f_FindEqual("RaceApp") != nullptr) + umint(Installed.f_FindEqual("raceapp") != nullptr), ==, 1u);
		}

		// The removal deletes the data disk of a VM environment on request, with the rest of the environment storage
		{
			DMibTestPath("Remove Deleting VM Data");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_VM;
			Settings.m_VMImage = CStr("DeleteImage");
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("DeleteVMEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			CStr StorageDirectory = AppManagerInfo.m_RootDirectory / ".Environment/DeleteVMEnv";
			co_await
				(
					g_Dispatch / [StorageDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(StorageDirectory);
						CFile::fs_WriteStringToFile(StorageDirectory / "DataDisk.img", "Data");
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating the data disk")
			;

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--environment-remove", "--name", "DeleteVMEnv", "--delete-application-data"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing environment")
			;

			DMibExpectFalse(CFile::fs_FileExists(StorageDirectory / "DataDisk.img"));
			DMibExpectFalse(CFile::fs_FileExists(StorageDirectory));

			auto Environments = co_await fGetEnvironments();
			DMibExpectTrue(Environments.f_FindEqual("DeleteVMEnv") == nullptr);
		}

		// Container environments keep the application files in storage they manage, which the removal keeps by default
		{
			DMibTestPath("Remove Keeping Environment Data");

			CStr Output = co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--environment-remove", "--name", "ContainerEnv"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing environment")
			;

			DMibExpectTrue(Output.f_Find("Kept the application data of environment 'ContainerEnv'") >= 0);

			auto Environments = co_await fGetEnvironments();
			DMibExpectTrue(Environments.f_FindEqual("ContainerEnv") == nullptr);
		}

		co_return {};
	};

	DMibTestSuite("Local Launch") -> TCFuture<void>
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

		CAppManagerTestHelper AppManagerTestHelper("AppManagerEnvironmentLaunchTests", Options, g_Timeout);

		auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

		co_await AppManagerTestHelper.f_Setup(1);

		auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
		auto &Interface = AppManagerInfo.m_Interface;

		auto fGetEnvironments = [&]() -> TCUnsafeFuture<TCMap<CStr, CAppManagerInterface::CEnvironmentInfo>>
			{
				co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetEnvironments)().f_Timeout(g_Timeout, "Timed out enumerating environments");
			}
		;

		auto fGetInstalled = [&]() -> TCUnsafeFuture<TCMap<CStr, CAppManagerInterface::CApplicationInfo>>
			{
				co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetInstalled)().f_Timeout(g_Timeout, "Timed out enumerating applications");
			}
		;

		{
			DMibTestPath("Initial Host Launch");

			auto Installed = co_await fGetInstalled();
			auto pTestApp = Installed.f_FindEqual("TestApp");
			DMibExpectTrue(pTestApp != nullptr);
			DMibExpect(pTestApp->m_Status, ==, "Launched");
		}

		{
			DMibTestPath("Install Agent Application");

			CStr AgentSourceDirectory = AppManagerTestHelper.f_RootDirectory() / "AgentSource";
			CStr AppManagerExecutable = AppManagerInfo.m_RootDirectory / "AppManager";
#ifdef DPlatformFamily_Windows
			AppManagerExecutable += ".exe";
#endif

			co_await
				(
					g_Dispatch / [AgentSourceDirectory, AppManagerExecutable]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(AgentSourceDirectory);
						CFile::fs_CopyFile(AppManagerExecutable, AgentSourceDirectory / CFile::fs_GetFile(AppManagerExecutable));
						CFile::fs_WriteStringToFile(AgentSourceDirectory / "NotAgentFile.txt", "Not an agent file");
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent source directory")
			;

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", AgentSourceDirectory, "--name", "Agent"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding agent application")
			;
		}

		{
			DMibTestPath("Add Environment");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			Settings.m_AutoStart = false;
			Settings.m_AgentApplication = CStr("Agent");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("TestEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");
		}

		{
			DMibTestPath("Launch In Environment");

			CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("TestEnv");

			// A change that waits for the move applies to the settings the move leaves
			CAppManagerInterface::CApplicationSettings OverlappingSettings;
			OverlappingSettings.m_UpdateGroup = CStr("Overlapping");

			TCFuture<void> MoveFuture = Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, Settings);
			TCFuture<void> OverlappingFuture = Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, OverlappingSettings);

			co_await fg_Move(MoveFuture).f_Timeout(g_Timeout, "Timed out changing application settings");
			co_await fg_Move(OverlappingFuture).f_Timeout(g_Timeout, "Timed out changing application settings");

			auto Installed = co_await fGetInstalled();
			auto pTestApp = Installed.f_FindEqual("TestApp");
			DMibExpectTrue(pTestApp != nullptr);
			DMibExpect(pTestApp->m_Status, ==, "Launched");
			DMibExpect(pTestApp->m_LaunchEnvironment, ==, "TestEnv");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Running");
			DMibExpectTrue(pEnvironment->m_Applications.f_FindEqual("TestApp") != nullptr);
		}

		// Only the files the agent runs are installed for the environment, and the agent writes its state beside them
		{
			DMibTestPath("Agent Files");

			CStr StorageDirectory = AppManagerInfo.m_RootDirectory / ".Environment/TestEnv";

			TCSet<CStr> AgentFileNames;
			for (auto &File : CFile::fs_FindFiles(StorageDirectory / "Agent/*", EFileAttrib_File | EFileAttrib_Directory, true, false))
				AgentFileNames[CFile::fs_GetFile(File)];

			DMibExpectTrue(AgentFileNames.f_FindEqual("AppManagerSettings.json") != nullptr);
			DMibExpectTrue(AgentFileNames.f_FindEqual("NotAgentFile.txt") == nullptr);
			DMibExpectTrue(AgentFileNames.f_FindEqual("TrustDatabase.AppManager") == nullptr);
			DMibExpectTrue(CFile::fs_FileExists(StorageDirectory / "State/TrustDatabase.AppManager", EFileAttrib_Directory | EFileAttrib_File));
		}

		{
			DMibTestPath("Stop And Start");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Stop)("TestApp").f_Timeout(g_Timeout, "Timed out stopping application");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_Start)("TestApp").f_Timeout(g_Timeout, "Timed out starting application");

			auto Installed = co_await fGetInstalled();
			auto pTestApp = Installed.f_FindEqual("TestApp");
			DMibExpectTrue(pTestApp != nullptr);
			DMibExpect(pTestApp->m_Status, ==, "Launched");

			auto StartAgainResult = co_await Interface.f_CallActor(&CAppManagerInterface::f_Start)("TestApp").f_Timeout(g_Timeout, "Timed out starting application").f_Wrap();
			DMibExpectFalse(StartAgainResult);
			if (!StartAgainResult)
				DMibExpect(StartAgainResult.f_GetExceptionStr().f_Find("Application already started"), >=, 0);
		}

		{
			DMibTestPath("Stop Environment");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStop)("TestEnv").f_Timeout(g_Timeout, "Timed out stopping environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Stopped");
		}

		{
			DMibTestPath("Application Start Keeps Environment Stopped");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_Start)("TestApp").f_Timeout(g_Timeout, "Timed out starting application").f_Wrap();
			DMibExpectFalse(Result);

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Stopped");
		}

		{
			DMibTestPath("Environment Start Relaunches Applications");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("TestEnv").f_Timeout(g_Timeout, "Timed out starting environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Running");

			CStr Status;
			NTime::CStopwatch Stopwatch{true};
			while (true)
			{
				auto Installed = co_await fGetInstalled();
				if (auto pTestApp = Installed.f_FindEqual("TestApp"))
					Status = pTestApp->m_Status;

				if (Status == "Launched" || Stopwatch.f_GetTime() > g_Timeout)
					break;

				co_await fg_Timeout(0.1);
			}

			DMibExpect(Status, ==, "Launched");
		}

		{
			DMibTestPath("Manual Stop During Restart Wins");

			TCFuture<void> RestartFuture = Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRestart)("TestEnv");

			// The restart has begun its stop once the application leaves Launched; it can have progressed further when the manual stop arrives
			CStr Status = "Launched";
			NTime::CStopwatch Stopwatch{true};
			while (Status == "Launched" && Stopwatch.f_GetTime() < g_Timeout)
			{
				co_await fg_Timeout(0.01);

				auto Installed = co_await fGetInstalled();
				if (auto pTestApp = Installed.f_FindEqual("TestApp"))
					Status = pTestApp->m_Status;
			}

			DMibExpect(Status, !=, "Launched");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStop)("TestEnv").f_Timeout(g_Timeout, "Timed out stopping environment");

			auto RestartResult = co_await fg_Move(RestartFuture).f_Timeout(g_Timeout, "Timed out restarting environment").f_Wrap();
			if (!RestartResult)
				DMibExpect(RestartResult.f_GetExceptionStr().f_Find("Environment 'TestEnv' was stopped"), >=, 0);

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("TestEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Stopped");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("TestEnv").f_Timeout(g_Timeout, "Timed out starting environment");

			Stopwatch.f_Start();
			while (true)
			{
				auto Installed = co_await fGetInstalled();
				if (auto pTestApp = Installed.f_FindEqual("TestApp"))
					Status = pTestApp->m_Status;

				if (Status == "Launched" || Stopwatch.f_GetTime() > g_Timeout)
					break;

				co_await fg_Timeout(0.1);
			}

			DMibExpect(Status, ==, "Launched");
		}

		{
			DMibTestPath("Back To Host Launch");

			CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, Settings).f_Timeout(g_Timeout, "Timed out changing application settings");

			auto Installed = co_await fGetInstalled();
			auto pTestApp = Installed.f_FindEqual("TestApp");
			DMibExpectTrue(pTestApp != nullptr);
			DMibExpect(pTestApp->m_Status, ==, "Launched");
			DMibExpect(pTestApp->m_LaunchEnvironment, ==, "");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv").f_Timeout(g_Timeout, "Timed out removing environment");
		}

		{
			DMibTestPath("Confined To Parent Application");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			Settings.m_AutoStart = false;
			Settings.m_AgentApplication = CStr("Agent");
			Settings.m_ParentApplication = CStr("Agent");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ConfinedEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("ConfinedEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_ParentApplication, ==, "Agent");
		}

		{
			DMibTestPath("Launch Creates Agent Root Under Parent");

			CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("ConfinedEnv");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, Settings).f_Timeout(g_Timeout, "Timed out changing application settings");

			auto Installed = co_await fGetInstalled();
			auto pTestApp = Installed.f_FindEqual("TestApp");
			DMibExpectTrue(pTestApp != nullptr);
			DMibExpect(pTestApp->m_Status, ==, "Launched");

			CStr ConfinedRoot = AppManagerInfo.m_RootDirectory / "App/Agent/.Environment/ConfinedEnv";
			CStr UnconfinedRoot = AppManagerInfo.m_RootDirectory / ".Environment/ConfinedEnv";
			DMibExpectTrue(CFile::fs_FileExists(ConfinedRoot, EFileAttrib_Directory));
			DMibExpectFalse(CFile::fs_FileExists(UnconfinedRoot, EFileAttrib_Directory));
		}

		{
			DMibTestPath("Parent Application Remove Protected");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_Remove)("Agent").f_Timeout(g_Timeout, "Timed out removing application").f_Wrap();
			DMibExpectFalse(Result);
		}

		{
			DMibTestPath("Confined Cleanup");

			CAppManagerInterface::CApplicationChangeSettings ChangeSettings;
			CAppManagerInterface::CApplicationSettings Settings;
			Settings.m_LaunchEnvironment = CStr("");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, Settings).f_Timeout(g_Timeout, "Timed out changing application settings");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("ConfinedEnv").f_Timeout(g_Timeout, "Timed out removing environment");
		}

		co_return {};
	};

#ifndef DPlatformFamily_Windows
	DMibTestSuite("Application Files Below Links")
	{
		CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerEnvironmentFiles";
		CStr ApplicationDirectory = RootDirectory / "App";
		CStr OutsideDirectory = RootDirectory / "Outside";

		fg_TestAddCleanupPath(RootDirectory);

		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(ApplicationDirectory);
		CFile::fs_CreateDirectory(OutsideDirectory);
		CFile::fs_WriteStringToFile(OutsideDirectory / "Data.txt", "Outside", false);
		CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);

		{
			DMibTestPath("Delete Below Link");

			fg_AppManager_DeleteApplicationFile(ApplicationDirectory, "Linked/Data.txt");
			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Data.txt", EFileAttrib_File));
		}

		{
			DMibTestPath("Delete Directory Link");

			fg_AppManager_DeleteApplicationFile(ApplicationDirectory, "Linked");
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Linked", EFileAttrib_Link));
			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Data.txt", EFileAttrib_File));
		}

		{
			DMibTestPath("Create Replaces Link");

			CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			fg_AppManager_CreateApplicationDirectory(ApplicationDirectory, "Linked/Sub");
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Linked", EFileAttrib_Link));
			DMibExpectTrue(CFile::fs_FileExists(ApplicationDirectory / "Linked/Sub", EFileAttrib_Directory));
			DMibExpectFalse(CFile::fs_FileExists(OutsideDirectory / "Sub"));
			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Data.txt", EFileAttrib_File));
		}
	};

	DMibTestSuite("Application Owners Below Links")
	{
		CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerApplicationOwners";
		CStr ApplicationDirectory = RootDirectory / "App";
		CStr OutsideDirectory = RootDirectory / "Outside";

		fg_TestAddCleanupPath(RootDirectory);

		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(ApplicationDirectory / "A/B");
		CFile::fs_CreateDirectory(OutsideDirectory / "Sub");
		CFile::fs_WriteStringToFile(ApplicationDirectory / "A/B/Executable", "", false);
		CFile::fs_WriteStringToFile(OutsideDirectory / "Executable", "", false);
		CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);
		CFile::fs_CreateSymbolicLink(OutsideDirectory / "Executable", ApplicationDirectory / "LinkedExecutable", EFileAttrib_File, ESymbolicLinkFlag_None);
		CFile::fs_CreateDirectory(ApplicationDirectory / "Real");
		CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "Real/Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);

		{
			DMibTestPath("Low Port Executables");

			DMibExpect(fg_AppManager_LowPortExecutablePath(ApplicationDirectory, "A/B/Executable"), ==, ApplicationDirectory / "A/B/Executable");
			DMibExpect(fg_AppManager_LowPortExecutablePath(ApplicationDirectory, "Linked/Executable"), ==, "");
			DMibExpect(fg_AppManager_LowPortExecutablePath(ApplicationDirectory, "LinkedExecutable"), ==, "");
			DMibExpect(fg_AppManager_LowPortExecutablePath(ApplicationDirectory, "Missing"), ==, "");
		}

		// A group of this user that the directories don't have shows which directories the change reaches
		CStr CurrentGroup = CFile::fs_GetGroup(OutsideDirectory / "Sub");
		CStr OtherGroup;
		{
			gid_t Groups[256];
			int nGroups = ::getgroups(256, Groups);
			for (int iGroup = 0; iGroup < nGroups && OtherGroup.f_IsEmpty(); ++iGroup)
			{
				NMib::NPlatform::CGetGrGidState State;
				if (group *pGroup = NMib::NPlatform::fg_Helper_GetGrGid(Groups[iGroup], State); pGroup && CurrentGroup != pGroup->gr_name)
					OtherGroup = pGroup->gr_name;
			}
		}

		{
			DMibTestPath("Owners");

			if (OtherGroup.f_IsEmpty())
				DMibConOut("Owners skipped: the user is in no other group\n");
			else
			{
				fg_AppManager_SetApplicationDirectoryOwners(ApplicationDirectory, ApplicationDirectory, {"A/B", "Linked/Sub", "Linked", "Real/Linked/Sub"}, "", OtherGroup);

				// The real directory above a link changes owner, the link and what it leads to don't
				DMibExpect(CFile::fs_GetGroup(ApplicationDirectory / "Real"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(ApplicationDirectory / "A/B"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(ApplicationDirectory / "A"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(ApplicationDirectory), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(OutsideDirectory / "Sub"), ==, CurrentGroup);
				DMibExpect(CFile::fs_GetGroup(OutsideDirectory), ==, CurrentGroup);
			}
		}

		{
			DMibTestPath("Linked Application Directory");

			CStr MovedDirectory = RootDirectory / "Moved";
			CStr LinkedApplicationDirectory = RootDirectory / "LinkedApp";
			CFile::fs_CreateDirectory(MovedDirectory / "A");
			CFile::fs_CreateSymbolicLink(OutsideDirectory, MovedDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);
			CFile::fs_CreateSymbolicLink("Moved", LinkedApplicationDirectory, EFileAttrib_Directory, ESymbolicLinkFlag_None);

			CStr Group = CFile::fs_GetGroup(MovedDirectory);
			fg_AppManager_SetApplicationDirectoryOwners(LinkedApplicationDirectory, LinkedApplicationDirectory, {"A", "Linked/Sub"}, "", Group);

			DMibExpect(CFile::fs_GetGroup(MovedDirectory / "A"), ==, Group);
			DMibExpect(CFile::fs_GetGroup(MovedDirectory), ==, Group);
		}

		// The file system resolves a .. in a link target after the links before it, unlike a textual expansion of the path
		{
			DMibTestPath("Linked Application Directory Through Link And Parent");

			CFile::fs_CreateDirectory(RootDirectory / "Real/Sub");
			CFile::fs_CreateDirectory(RootDirectory / "Real/Apps/App/A");
			CFile::fs_CreateSymbolicLink("Real/Sub", RootDirectory / "Via", EFileAttrib_Directory, ESymbolicLinkFlag_None);
			CFile::fs_CreateSymbolicLink("Via/../Apps/App", RootDirectory / "ViaApp", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			// The path that normalizing Via/.. as text gives leads to another directory
			CFile::fs_CreateDirectory(RootDirectory / "Decoy/A");
			CFile::fs_CreateDirectory(RootDirectory / "Apps");
			CFile::fs_CreateSymbolicLink("../Decoy", RootDirectory / "Apps/App", EFileAttrib_Directory, ESymbolicLinkFlag_None);
			CStr DecoyGroup = CFile::fs_GetGroup(RootDirectory / "Decoy/A");

			if (OtherGroup.f_IsEmpty())
				DMibConOut("Owners through link and parent skipped: the user is in no other group\n");
			else
			{
				fg_AppManager_SetApplicationDirectoryOwners(RootDirectory / "ViaApp", RootDirectory / "ViaApp", {"A"}, "", OtherGroup);

				DMibExpect(CFile::fs_GetGroup(RootDirectory / "Real/Apps/App/A"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(RootDirectory / "Real/Apps/App"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(RootDirectory / "Decoy/A"), ==, DecoyGroup);
			}
		}

		// The account of the parent application owns the directory that a child application is in, so a link at the child's directory isn't followed
		{
			DMibTestPath("Linked Child Application Directory");

			CStr ParentDirectory = RootDirectory / "Parent";
			CStr ChildTarget = RootDirectory / "ChildTarget";
			CFile::fs_CreateDirectory(ParentDirectory);
			CFile::fs_CreateDirectory(ChildTarget / "A");
			CFile::fs_CreateSymbolicLink(ChildTarget, ParentDirectory / "Child", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			if (OtherGroup.f_IsEmpty())
				DMibConOut("Linked Child Application Directory skipped: the user is in no other group\n");
			else
			{
				CStr TargetGroup = CFile::fs_GetGroup(ChildTarget);
				CStr ParentGroup = CFile::fs_GetGroup(ParentDirectory);
				fg_AppManager_SetApplicationDirectoryOwners(ParentDirectory, ParentDirectory / "Child", {"A", ".home"}, "", OtherGroup);

				DMibExpect(CFile::fs_GetGroup(ChildTarget), ==, TargetGroup);
				DMibExpect(CFile::fs_GetGroup(ChildTarget / "A"), ==, TargetGroup);
				DMibExpect(CFile::fs_GetGroup(ParentDirectory), ==, ParentGroup);
			}
		}

		// A real child directory gets the owners, and the directory of the parent keeps its own
		{
			DMibTestPath("Child Application Directory");

			CStr ParentDirectory = RootDirectory / "Parent";
			CFile::fs_DeleteFile(ParentDirectory / "Child");
			CFile::fs_CreateDirectory(ParentDirectory / "Child/A");

			if (!OtherGroup.f_IsEmpty())
			{
				CStr ParentGroup = CFile::fs_GetGroup(ParentDirectory);
				fg_AppManager_SetApplicationDirectoryOwners(ParentDirectory, ParentDirectory / "Child", {"A"}, "", OtherGroup);

				DMibExpect(CFile::fs_GetGroup(ParentDirectory / "Child/A"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(ParentDirectory / "Child"), ==, OtherGroup);
				DMibExpect(CFile::fs_GetGroup(ParentDirectory), ==, ParentGroup);
			}
		}
	};

	DMibTestSuite("Replace Application Files Below Links")
	{
		CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerReplaceApplicationFiles";
		CStr ApplicationDirectory = RootDirectory / "App";
		CStr SourceDirectory = RootDirectory / "Source";
		CStr OutsideDirectory = RootDirectory / "Outside";

		fg_TestAddCleanupPath(RootDirectory);

		if (CFile::fs_FileExists(RootDirectory))
			CFile::fs_DeleteDirectoryRecursive(RootDirectory);

		CFile::fs_CreateDirectory(ApplicationDirectory);
		CFile::fs_CreateDirectory(SourceDirectory / "Linked");
		CFile::fs_CreateDirectory(OutsideDirectory);
		CFile::fs_WriteStringToFile(OutsideDirectory / "Data.txt", "Outside", false);
		CFile::fs_WriteStringToFile(ApplicationDirectory / "Old.txt", "Old", false);
		CFile::fs_WriteStringToFile(SourceDirectory / "Linked/New.txt", "New", false);
		CFile::fs_WriteStringToFile(SourceDirectory / "Top.txt", "Top", false);
		CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);
		CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "OldLink", EFileAttrib_Directory, ESymbolicLinkFlag_None);

		// Removed.txt stands for a file that an update script removed from the unpack, so it is not installed
		auto Installed = fg_AppManager_ReplaceApplicationFiles
			(
				ApplicationDirectory
				, SourceDirectory
				, {"Old.txt", "OldLink", "Linked/Data.txt"}
				, {"Linked", "Linked/New.txt", "Top.txt", "Removed.txt"}
			)
		;
		DMibExpect(Installed, ==, fg_CreateVector<CStr>("Linked", "Linked/New.txt", "Top.txt"));

		{
			DMibTestPath("Old Files");

			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Old.txt"));
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "OldLink", EFileAttrib_Link));
			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Data.txt", EFileAttrib_File));
		}

		{
			DMibTestPath("New Files");

			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Linked", EFileAttrib_Link));
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Linked/New.txt"), ==, "New");
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Top.txt"), ==, "Top");
			DMibExpectFalse(CFile::fs_FileExists(OutsideDirectory / "New.txt"));
		}

		{
			DMibTestPath("Unpack Below Link");

			CStr UnpackDirectory = ApplicationDirectory / "TempVersion/Unpack";
			CFile::fs_CreateDirectory(UnpackDirectory);
			CFile::fs_CreateSymbolicLink(OutsideDirectory, UnpackDirectory / "Linked", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			DMibExpectException
				(
					fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, UnpackDirectory, {}, {"Linked/Data.txt"})
					, DMibErrorInstance("The unpacked application file 'Linked/Data.txt' is below a link")
				)
			;

			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Data.txt", EFileAttrib_File));
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Linked/Data.txt"));
		}

		{
			DMibTestPath("Unpack Root Below Link");

			CFile::fs_DeleteFile(ApplicationDirectory / "TempVersion/Unpack/Linked");
			CFile::fs_DeleteDirectoryRecursive(ApplicationDirectory / "TempVersion");
			CFile::fs_CreateDirectory(OutsideDirectory / "Unpack");
			CFile::fs_WriteStringToFile(OutsideDirectory / "Unpack/Secret.txt", "Secret", false);
			CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "TempVersion", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			DMibExpectException
				(
					fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, ApplicationDirectory / "TempVersion/Unpack", {}, {"Secret.txt"})
					, DMibErrorInstance("The unpacked application file 'Secret.txt' is below a link")
				)
			;

			DMibExpectTrue(CFile::fs_FileExists(OutsideDirectory / "Unpack/Secret.txt", EFileAttrib_File));
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Secret.txt"));
		}

		{
			DMibTestPath("Empty Directories");

			CStr EmptySource = RootDirectory / "EmptySource";
			CFile::fs_CreateDirectory(EmptySource / "Empty");
			CFile::fs_CreateDirectory(EmptySource / "InTheWay");
			CFile::fs_WriteStringToFile(ApplicationDirectory / "InTheWay", "Application", false);

			DMibExpect(fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, EmptySource, {}, {"Empty"}), ==, fg_CreateVector<CStr>("Empty"));
			DMibExpectTrue(CFile::fs_FileExists(ApplicationDirectory / "Empty", EFileAttrib_Directory));

			DMibExpectException
				(
					fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, EmptySource, {}, {"InTheWay"})
					, DMibErrorInstance("The application file 'InTheWay' is where the update has a directory")
				)
			;
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "InTheWay"), ==, "Application");
		}

		{
			DMibTestPath("Empty Directory Below Link");

			// The link replaced by a directory, not the file beyond it, decides
			CStr EmptySource = RootDirectory / "EmptyBelowLinkSource";
			CFile::fs_CreateDirectory(EmptySource / "LinkedParent/Empty");
			CFile::fs_WriteStringToFile(OutsideDirectory / "Empty", "Outside", false);
			CFile::fs_CreateSymbolicLink(OutsideDirectory, ApplicationDirectory / "LinkedParent", EFileAttrib_Directory, ESymbolicLinkFlag_None);

			DMibExpect(fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, EmptySource, {}, {"LinkedParent/Empty"}), ==, fg_CreateVector<CStr>("LinkedParent/Empty"));
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "LinkedParent", EFileAttrib_Link));
			DMibExpectTrue(CFile::fs_FileExists(ApplicationDirectory / "LinkedParent/Empty", EFileAttrib_Directory));
			DMibExpect(CFile::fs_ReadStringFromFile(OutsideDirectory / "Empty"), ==, "Outside");
		}

		{
			DMibTestPath("File Kept By Update Script");

			// The update script removed the copy of Kept.txt in the unpack to keep the copy of the previous version
			CStr KeptSource = RootDirectory / "KeptSource";
			CFile::fs_CreateDirectory(KeptSource);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "Kept.txt", "Live", false);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "Dropped.txt", "Live", false);

			DMibExpect(fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, KeptSource, {"Kept.txt", "Dropped.txt"}, {"Kept.txt"}), ==, fg_CreateVector<CStr>("Kept.txt"));
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Kept.txt"), ==, "Live");
			DMibExpectFalse(CFile::fs_FileExists(ApplicationDirectory / "Dropped.txt"));
		}

		{
			DMibTestPath("Kept File Without Copy");

			// The script removed the copy in the unpack, and the previous version has none left either
			CStr KeptSource = RootDirectory / "KeptWithoutCopySource";
			CFile::fs_CreateDirectory(KeptSource);

			DMibExpect(fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, KeptSource, {"Gone.txt"}, {"Gone.txt"}), ==, TCVector<CStr>());
		}

		{
			DMibTestPath("Directory Conflict Keeps Old Files");

			CStr ConflictSource = RootDirectory / "ConflictSource";
			CFile::fs_CreateDirectory(ConflictSource / "Blocked");
			CFile::fs_CreateDirectory(ConflictSource / "Nested");
			CFile::fs_WriteStringToFile(ConflictSource / "Nested/File.txt", "New", false);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "Blocked", "Application", false);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "Old.txt", "Old", false);

			DMibExpectException
				(
					fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, ConflictSource, {"Old.txt"}, {"Blocked"})
					, DMibErrorInstance("The application file 'Blocked' is where the update has a directory")
				)
			;
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Old.txt"), ==, "Old");

			{
				DMibTestPath("Parent Of A File");

				CFile::fs_WriteStringToFile(ApplicationDirectory / "Nested", "Application", false);
				DMibExpectException
					(
						fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, ConflictSource, {"Old.txt"}, {"Nested/File.txt"})
						, DMibErrorInstance("The application file 'Nested' is where the update has a directory")
					)
				;
				DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Old.txt"), ==, "Old");
			}
		}

		{
			DMibTestPath("Entry Removed By The Scripts");

			// The application's file stays where the removed entry would have had its directory
			CStr RemovedSource = RootDirectory / "RemovedSource";
			CFile::fs_CreateDirectory(RemovedSource);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "Config", "Application", false);

			auto Installed = fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, RemovedSource, {}, {"Config/Default.json"});
			DMibExpectTrue(Installed.f_IsEmpty());
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Config"), ==, "Application");
		}

		{
			DMibTestPath("Old File Spelled Otherwise");

			// On a case-insensitive file system the old file is at the path of the new directory, which replaces it
			CStr CaseSource = RootDirectory / "CaseSource";
			CFile::fs_CreateDirectory(CaseSource / "Settings");
			CFile::fs_WriteStringToFile(CaseSource / "Settings/Default.json", "New", false);
			CFile::fs_WriteStringToFile(ApplicationDirectory / "settings", "Old", false);

			auto Installed = fg_AppManager_ReplaceApplicationFiles(ApplicationDirectory, CaseSource, {"settings"}, {"Settings/Default.json"});
			DMibExpect(Installed.f_GetLen(), ==, 1u);
			DMibExpect(CFile::fs_ReadStringFromFile(ApplicationDirectory / "Settings/Default.json"), ==, "New");
		}
	};

	DMibTestSuite("Environment Socket Directory")
	{
		{
			DMibTestPath("Root Directory");

			auto Container = fg_AppManager_EnvironmentSocketDirectory("/opt/AppManager", "Env", true, false);
			DMibExpect(Container.m_SharedDirectory, ==, "/opt/AppManager/Sockets");
			DMibExpect(CFile::fs_GetPath(Container.m_EnvironmentDirectory), ==, Container.m_SharedDirectory);
			DMibExpect(Container.m_SocketDirectory, ==, Container.m_EnvironmentDirectory / "Socket");
			DMibExpect(Container.m_SocketPath, ==, Container.m_SocketDirectory / "Agent.socket");

			auto VM = fg_AppManager_EnvironmentSocketDirectory("/opt/AppManager", "Env", false, true);
			DMibExpect(VM.m_EnvironmentDirectory, ==, Container.m_EnvironmentDirectory);
			DMibExpect(VM.m_SocketDirectory, ==, VM.m_EnvironmentDirectory);

			auto Other = fg_AppManager_EnvironmentSocketDirectory("/opt/AppManager", "OtherEnv", false, false);
			DMibExpect(Other.m_EnvironmentDirectory, !=, VM.m_EnvironmentDirectory);
		}

		{
			DMibTestPath("Long Root Directory");

			CStr RootDirectory = "/opt";
			for (umint iComponent = 0; iComponent < 10; ++iComponent)
				RootDirectory = RootDirectory / "LongDirectoryName";

			auto AsRoot = fg_AppManager_EnvironmentSocketDirectory(RootDirectory, "Env", true, true);
			DMibExpect(AsRoot.m_SharedDirectory, ==, "/var/run/MalterlibAppManager");
			DMibExpect(CFile::fs_GetPath(AsRoot.m_EnvironmentDirectory), ==, AsRoot.m_SharedDirectory);

			auto AsOtherUser = fg_AppManager_EnvironmentSocketDirectory(RootDirectory, "Env", true, false);
			DMibExpect(AsOtherUser.m_SharedDirectory, ==, "");
			DMibExpect(CFile::fs_GetPath(AsOtherUser.m_EnvironmentDirectory), ==, CFile::fs_GetRawTemporaryDirectory());
			DMibExpect(AsOtherUser.m_EnvironmentDirectory, !=, fg_AppManager_EnvironmentSocketDirectory(RootDirectory + "B", "Env", true, false).m_EnvironmentDirectory);
		}

		{
			DMibTestPath("Prepare");

			CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerEnvironmentSocketDirectory";

			fg_TestAddCleanupPath(RootDirectory);

			if (CFile::fs_FileExists(RootDirectory))
				CFile::fs_DeleteDirectoryRecursive(RootDirectory);

			CAppManagerEnvironmentSocketDirectory Directory;
			Directory.m_SharedDirectory = RootDirectory / "Shared";
			Directory.m_EnvironmentDirectory = Directory.m_SharedDirectory / "Env";
			Directory.m_SocketDirectory = Directory.m_EnvironmentDirectory / "Socket";

			constexpr EFileAttrib c_Traversable = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_GroupExecute | EFileAttrib_EveryoneExecute;
			constexpr EFileAttrib c_Private = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute;

			{
				DMibTestPath("Created");

				fg_AppManager_PrepareEnvironmentSocketDirectory(Directory);
				DMibExpect(CFile::fs_GetAttributes(Directory.m_SharedDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Traversable);
				DMibExpect(CFile::fs_GetAttributes(Directory.m_EnvironmentDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Private);
				DMibExpect(CFile::fs_GetAttributes(Directory.m_SocketDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Traversable);
			}

			{
				DMibTestPath("Existing");

				// An existing environment directory that others can traverse is restricted, never opened up
				CFile::fs_SetAttributes(Directory.m_EnvironmentDirectory, EFileAttrib_UnixAttributesValid | c_Traversable | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead);
				fg_AppManager_PrepareEnvironmentSocketDirectory(Directory);
				DMibExpect(CFile::fs_GetAttributes(Directory.m_EnvironmentDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Private);
			}

			{
				DMibTestPath("Link");

				CStr OutsideDirectory = RootDirectory / "Outside";
				CFile::fs_CreateDirectory(OutsideDirectory / "Socket");
				CFile::fs_SetAttributes(OutsideDirectory, EFileAttrib_UnixAttributesValid | c_Traversable | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead);
				CFile::fs_DeleteDirectoryRecursive(Directory.m_EnvironmentDirectory);
				CFile::fs_CreateSymbolicLink(OutsideDirectory, Directory.m_EnvironmentDirectory, EFileAttrib_Directory, ESymbolicLinkFlag_None);

				DMibExpectException
					(
						fg_AppManager_PrepareEnvironmentSocketDirectory(Directory)
						, DMibErrorInstance("Environment socket directory '{}' is not a directory"_f << Directory.m_EnvironmentDirectory)
					)
				;
				DMibExpect(CFile::fs_GetAttributes(OutsideDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Traversable | EFileAttrib_GroupRead | EFileAttrib_EveryoneRead);
			}
		}
	};
#endif

	DMibTestSuite("Docker Launch") -> TCFuture<void>
	{
		// The agent socket directory bind mount needs a Linux host
#ifdef DPlatformFamily_Linux
		constexpr bool c_bSupportedHost = true;
#else
		constexpr bool c_bSupportedHost = false;
#endif
		if (!c_bSupportedHost)
		{
			DMibConOut("Docker Launch skipped: it needs a Linux host\n");
			co_return {};
		}

		// Setting the executable asks for the run, so from here on a missing docker fails the suite
		CStr LinuxAgentExecutable = fg_GetSys()->f_GetEnvironmentVariable("MalterlibTestLinuxAgentExecutable");
		if (LinuxAgentExecutable.f_IsEmpty())
		{
			DMibConOut("Docker Launch skipped: MalterlibTestLinuxAgentExecutable does not name a Linux AppManager executable to run as the agent\n");
			co_return {};
		}

		{
			CProcessLaunchParams Params = CProcessLaunchParams::fs_LaunchExecutable("docker", {"version"}, CStr(), {});
			Params.m_bAllowExecutableLocate = true;
			Params.m_bMergeEnvironment = true;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(Params, CProcessLaunchActor::ESimpleLaunchFlag_None)).f_Wrap();
			if (!Result)
				co_return DMibErrorInstance("Docker is not available: {}"_f << Result.f_GetExceptionStr());
			if (Result->m_ExitCode != 0)
				co_return DMibErrorInstance("Docker is not available: 'docker version' exited with {}: {}"_f << Result->m_ExitCode << CStr(Result->f_GetCombinedOut().f_Trim()));
		}

		// A reachable docker daemon can run on another host; the container needs the agent and data directories of this host
		{
			CStr ProbeDirectory = CFile::fs_GetProgramDirectory() / "AppManagerEnvironmentDockerProbe";
			CStr ProbeFile = ProbeDirectory / "probe";

			NTest::fg_TestAddCleanupPath(ProbeDirectory);

			co_await
				(
					g_Dispatch / [ProbeDirectory, ProbeFile]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						if (CFile::fs_FileExists(ProbeDirectory, EFileAttrib_Directory))
							CFile::fs_DeleteDirectoryRecursive(ProbeDirectory, true);

						CFile::fs_CreateDirectory(ProbeDirectory);
						CFile::fs_WriteStringToFile(ProbeFile, "probe", false);
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out writing probe")
			;

			CProcessLaunchParams Params = CProcessLaunchParams::fs_LaunchExecutable
				(
					"docker"
					, {"run", "--rm", "--volume", "{}:{}"_f << ProbeDirectory << ProbeDirectory, "ubuntu:24.04", "cat", ProbeFile}
					, CStr()
					, {}
				)
			;
			Params.m_bAllowExecutableLocate = true;
			Params.m_bMergeEnvironment = true;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(Params, CProcessLaunchActor::ESimpleLaunchFlag_None))
				.f_Timeout(300.0, "Timed out probing docker bind mount")
				.f_Wrap()
			;

			if (!Result)
				co_return DMibErrorInstance("The docker bind mount probe failed: {}"_f << Result.f_GetExceptionStr());
			if (Result->m_ExitCode != 0 || Result->f_GetStdOut().f_Trim() != "probe")
			{
				co_return DMibErrorInstance
					(
						"The docker daemon does not see the directories of this host through bind mounts: the probe exited with {}: {}"_f
						<< Result->m_ExitCode
						<< CStr(Result->f_GetCombinedOut().f_Trim())
					)
				;
			}
		}

		// A run that failed before removing its environment leaves files of the container's accounts in the environment data, which only a
		// container can delete, like the removal of the environment does
		{
			CStr PreviousRoot = CFile::fs_GetProgramDirectory() / "AppManagerEnvironmentDockerTests";

			TCVector<CStr> DataDirectories = co_await
				(
					g_Dispatch / [PreviousRoot]() -> TCFuture<TCVector<CStr>>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						TCVector<CStr> Directories;
						if (!CFile::fs_FileExists(PreviousRoot, EFileAttrib_Directory))
							co_return fg_Move(Directories);

						for (auto &AppManagerDirectory : CFile::fs_FindFiles(PreviousRoot / "*", EFileAttrib_Directory, false, false))
						{
							for (auto &EnvironmentDirectory : CFile::fs_FindFiles(AppManagerDirectory / ".Environment/*", EFileAttrib_Directory, false, false))
							{
								if (CFile::fs_FileExists(EnvironmentDirectory / "Data/Root", EFileAttrib_Directory))
									Directories.f_Insert(EnvironmentDirectory / "Data");
							}
						}

						co_return fg_Move(Directories);
					}
				)
				.f_Timeout(g_Timeout, "Timed out finding earlier environment data")
			;

			for (auto &DataDirectory : DataDirectories)
			{
				CProcessLaunchParams Params = CProcessLaunchParams::fs_LaunchExecutable
					(
						"docker"
						, {"run", "--rm", "--user", "0:0", "--network", "none", "--volume", "{}:/Data"_f << DataDirectory, "--entrypoint", "rm", "ubuntu:24.04", "-rf", "/Data/Root"}
						, CStr()
						, {}
					)
				;
				Params.m_bAllowExecutableLocate = true;
				Params.m_bMergeEnvironment = true;

				auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(Params, CProcessLaunchActor::ESimpleLaunchFlag_None))
					.f_Timeout(300.0, "Timed out deleting earlier environment data")
				;
				if (Result.m_ExitCode != 0)
					co_return DMibErrorInstance("Failed to delete the earlier environment data '{}': {}"_f << DataDirectory << CStr(Result.f_GetCombinedOut().f_Trim()));
			}
		}

		CAppManagerTestHelper::EOption Options
			= CAppManagerTestHelper::EOption_EnableVersionManager
			| CAppManagerTestHelper::EOption_DisablePatchMonitoring
			| CAppManagerTestHelper::EOption_DisableDiskMonitoring
			| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
			| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
		;

		if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
			Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

		CAppManagerTestHelper AppManagerTestHelper("AppManagerEnvironmentDockerTests", Options, g_Timeout);

		auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

		co_await AppManagerTestHelper.f_Setup(1);

		auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
		auto &Interface = AppManagerInfo.m_Interface;

		auto fGetEnvironments = [&]() -> TCUnsafeFuture<TCMap<CStr, CAppManagerInterface::CEnvironmentInfo>>
			{
				co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetEnvironments)().f_Timeout(g_Timeout, "Timed out enumerating environments");
			}
		;

		// Pull the image up front so the environment start is not dominated by the download
		{
			DMibTestPath("Pull Image");

			CProcessLaunchParams Params = CProcessLaunchParams::fs_LaunchExecutable("docker", {"pull", "ubuntu:24.04"}, CStr(), {});
			Params.m_bAllowExecutableLocate = true;
			Params.m_bMergeEnvironment = true;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(Params, CProcessLaunchActor::ESimpleLaunchFlag_None))
				.f_Timeout(600.0, "Timed out pulling image")
			;
			DMibExpect(Result.m_ExitCode, ==, 0u);
		}

		{
			DMibTestPath("Install Agent Application");

			CStr AgentSourceDirectory = AppManagerTestHelper.f_RootDirectory() / "AgentSource";

			co_await
				(
					g_Dispatch / [AgentSourceDirectory, LinuxAgentExecutable]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(AgentSourceDirectory);
						CFile::fs_CopyFile(LinuxAgentExecutable, AgentSourceDirectory / "AppManager");
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent source directory")
			;

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", AgentSourceDirectory, "--name", "Agent"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding agent application")
			;
		}

		{
			DMibTestPath("Start Environment");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
			Settings.m_ContainerRuntime = CStr("Docker");
			Settings.m_ContainerImage = CStr("ubuntu:24.04");
			Settings.m_AgentApplication = CStr("Agent");
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("DockerEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("DockerEnv").f_Timeout(300.0, "Timed out starting docker environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("DockerEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Running");
		}

		{
			DMibTestPath("Stop Environment");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStop)("DockerEnv").f_Timeout(g_Timeout, "Timed out stopping environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("DockerEnv");
			DMibExpectTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Stopped");
		}

		// The container exists, which refuses changes of the runtime, and an empty runtime is the Docker runtime it was added with
		{
			DMibTestPath("Change To Same Runtime");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_ContainerRuntime = CStr("");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)("DockerEnv", Settings).f_Timeout(g_Timeout, "Timed out changing environment settings");
		}

		// The container writes the data as its own accounts, which the host need not be able to delete
		{
			DMibTestPath("Remove Deleting Environment Data");

			CStr StorageDirectory = AppManagerInfo.m_RootDirectory / ".Environment/DockerEnv";
			DMibExpectTrue(CFile::fs_FileExists(StorageDirectory / "Data/Root/Agent", EFileAttrib_Directory));

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--environment-remove", "--name", "DockerEnv", "--delete-application-data"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing environment")
			;

			DMibExpectFalse(CFile::fs_FileExists(StorageDirectory));

			auto Environments = co_await fGetEnvironments();
			DMibExpectTrue(Environments.f_FindEqual("DockerEnv") == nullptr);
		}

		co_return {};
	};

	DMibTestSuite("VM Launch") -> TCFuture<void>
	{
		DMibTestCategory("Provision Username")
		{
			DMibExpectTrue(fg_AppManager_IsValidVMProvisionUsername("admin"));
			DMibExpectTrue(fg_AppManager_IsValidVMProvisionUsername("_build-agent.2"));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername(""));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername("-oProxyCommand=touch /tmp/Owned;"));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername("2admin"));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername("admin@host"));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername("ad min"));
			DMibExpectFalse(fg_AppManager_IsValidVMProvisionUsername("abcdefghijklmnopqrstuvwxyzabcdefg"));
		};

		// A provisioned macOS guest, with Remote Login for the given account, which the AppManager sets the agent up through
		CStr GuestBundle = fg_GetSys()->f_GetEnvironmentVariable("MalterlibTestMacOSVMGuestBundle");
		CStr GuestUser = fg_GetSys()->f_GetEnvironmentVariable("MalterlibTestMacOSVMGuestUser");
		CStr GuestPassword = fg_GetSys()->f_GetEnvironmentVariable("MalterlibTestMacOSVMGuestPassword");
		if (GuestBundle.f_IsEmpty() || GuestUser.f_IsEmpty() || GuestPassword.f_IsEmpty())
		{
			DMibConOut
				(
					"VM Launch skipped: MalterlibTestMacOSVMGuestBundle, MalterlibTestMacOSVMGuestUser and MalterlibTestMacOSVMGuestPassword do not name "
					"a provisioned macOS guest\n"
				)
			;
			co_return {};
		}

#ifdef DPlatformFamily_macOS
		CAppManagerTestHelper::EOption Options
			= CAppManagerTestHelper::EOption_EnableVersionManager
			| CAppManagerTestHelper::EOption_DisablePatchMonitoring
			| CAppManagerTestHelper::EOption_DisableDiskMonitoring
			| CAppManagerTestHelper::EOption_DisableApplicationStatusSensors
			| CAppManagerTestHelper::EOption_DisableEncryptionStatusSensors
		;

		if (fg_TestReportFlags() & ETestReportFlag_EnableLogs)
			Options |= CAppManagerTestHelper::EOption_EnableOtherOutput;

		CAppManagerTestHelper AppManagerTestHelper("AppManagerEnvironmentVMTests", Options, g_Timeout);

		auto AsyncDestroy = co_await fg_AsyncDestroy(AppManagerTestHelper);

		co_await AppManagerTestHelper.f_Setup(1);

		auto &AppManagerInfo = *AppManagerTestHelper.m_pState->m_AppManagerInfos.f_FindAny();
		auto &Interface = AppManagerInfo.m_Interface;

		auto fGetEnvironments = [&]() -> TCUnsafeFuture<TCMap<CStr, CAppManagerInterface::CEnvironmentInfo>>
			{
				co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetEnvironments)().f_Timeout(g_Timeout, "Timed out enumerating environments");
			}
		;

		{
			DMibTestPath("Install Agent Application");

			CStr AgentSourceDirectory = AppManagerTestHelper.f_RootDirectory() / "AgentSource";
			CStr AgentExecutable = AppManagerInfo.m_RootDirectory / "AppManager";

			co_await
				(
					g_Dispatch / [AgentSourceDirectory, AgentExecutable]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(AgentSourceDirectory);
						CFile::fs_CopyFile(AgentExecutable, AgentSourceDirectory / "AppManager");
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating agent source directory")
			;

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--application-add", "--from-file", AgentSourceDirectory, "--name", "Agent"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out adding agent application")
			;
		}

		{
			DMibTestPath("Place Image");

			// A clone shares the blocks of the guest disk, so the guest can change it without changing the source bundle
			CStr ImageDirectory = AppManagerInfo.m_RootDirectory / ".VMImages/TestImage";

			co_await
				(
					g_Dispatch / [ImageDirectory]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_CreateDirectory(CFile::fs_GetPath(ImageDirectory));
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out creating the image directory")
			;

			CProcessLaunchParams Params = CProcessLaunchParams::fs_LaunchExecutable("/bin/cp", {"-c", "-R", GuestBundle, ImageDirectory}, CStr(), {});
			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(CProcessLaunchActor::CSimpleLaunch(Params, CProcessLaunchActor::ESimpleLaunchFlag_None))
				.f_Timeout(600.0, "Timed out cloning the guest bundle")
			;
			DMibAssert(CStr(Result.f_GetCombinedOut().f_Trim()), ==, "");
			DMibAssert(Result.m_ExitCode, ==, 0u);

			CEJsonSorted Provisioning;
			Provisioning["Username"] = GuestUser;
			Provisioning["Password"] = GuestPassword;
			Provisioning["FullName"] = GuestUser;
			Provisioning["AutoLogin"] = true;
			Provisioning["EnableRemoteLogin"] = true;

			co_await
				(
					g_Dispatch / [ImageDirectory, Contents = Provisioning.f_ToString()]() -> TCFuture<void>
					{
						co_await ECoroutineFlag_CaptureExceptions;

						CFile::fs_WriteStringToFile(ImageDirectory / "Provisioning.json", Contents, false);
						co_return {};
					}
				)
				.f_Timeout(g_Timeout, "Timed out writing the image provisioning")
			;
		}

		{
			DMibTestPath("Start Environment");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_VM;
			Settings.m_VMImage = CStr("TestImage");
			Settings.m_AgentApplication = CStr("Agent");
			Settings.m_AutoStart = false;

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("VMEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment");

			// The guest boots, the host installs the agent through SSH and the agent connects through its virtio socket
			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("VMEnv").f_Timeout(900.0, "Timed out starting VM environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("VMEnv");
			DMibAssertTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Running");
		}

		{
			DMibTestPath("Stop Environment");

			co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStop)("VMEnv").f_Timeout(300.0, "Timed out stopping environment");

			auto Environments = co_await fGetEnvironments();
			auto pEnvironment = Environments.f_FindEqual("VMEnv");
			DMibAssertTrue(pEnvironment != nullptr);
			DMibExpect(pEnvironment->m_Status, ==, "Stopped");
		}

		{
			DMibTestPath("Remove Deleting Environment Data");

			co_await AppManagerTestHelper.f_LaunchTool
				(
					AppManagerInfo.m_RootDirectory / "AppManager"
					, {"--environment-remove", "--name", "VMEnv", "--delete-application-data"}
					, AppManagerInfo.m_RootDirectory
				)
				.f_Timeout(g_Timeout, "Timed out removing environment")
			;

			auto Environments = co_await fGetEnvironments();
			DMibExpectTrue(Environments.f_FindEqual("VMEnv") == nullptr);
		}
#endif

		co_return {};
	};

	DMibTestSuite("Guest Text")
	{
		DMibExpect(fg_AppManager_SanitizeGuestText("Installing updates", 256), ==, "Installing updates");
		DMibExpect(fg_AppManager_SanitizeGuestText("Line\nTab\tEnd", 256), ==, "Line\nTab\tEnd");
		DMibExpect(fg_AppManager_SanitizeGuestText("\x1B[2JCleared\rBack\x7F", 256), ==, "?[2JCleared?Back?");
		DMibExpect(fg_AppManager_SanitizeGuestText("C1 \xC2\x9B" "2J", 256), ==, "C1 ?2J");
		DMibExpect(fg_AppManager_SanitizeGuestText("\xC3\xA5\xC3\xA4", 256), ==, "\xC3\xA5\xC3\xA4");

		// A cut keeps whole characters
		DMibExpect(fg_AppManager_SanitizeGuestText("ab\xC3\xA5", 3), ==, "ab");
		DMibExpect(fg_AppManager_SanitizeGuestText("abcdef", 3), ==, "abc");
	};

	DMibTestSuite("Agent AutoUpdate Config")
	{
		CEJsonSortedYaml HostConfig;
		HostConfig["AutoUpdate"]["SecurityUpdates"] = true;
		HostConfig["AutoUpdate"]["AutomaticReboot"] = true;

		{
			DMibTestPath("Container");

			CStr Config = fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType_Container, false, HostConfig);
			DMibAssertFalse(Config.f_IsEmpty());

			auto Json = CEJsonSorted::fs_FromString(Config);
			DMibExpectTrue(Json.f_GetMemberValue("SecurityUpdates", false).f_Boolean());
			DMibExpectTrue(Json.f_GetMemberValue("AutomaticReboot", false).f_Boolean());
		}

		{
			DMibTestPath("VM");
			DMibExpectFalse(fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType_VM, false, HostConfig).f_IsEmpty());
		}

		{
			DMibTestPath("Local");
			DMibExpectTrue(fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType_Local, false, HostConfig).f_IsEmpty());
		}

		{
			DMibTestPath("Read Only Container");
			DMibExpectTrue(fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType_Container, true, HostConfig).f_IsEmpty());
		}

		{
			DMibTestPath("No AutoUpdate");
			DMibExpectTrue(fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType_Container, false, CEJsonSortedYaml()).f_IsEmpty());
		}
	};

	DMibTestSuite("Container Arguments") -> TCFuture<void>
	{
		{
			DMibTestPath("Minimal");

			CAppManagerContainerLaunch Launch;
			Launch.m_ContainerName = "mib-env-TestEnv";
			Launch.m_Image = "ubuntu:24.04";
			Launch.m_Executable = "/opt/M/App/Agent/AppManager";
			Launch.m_Parameters = {"--daemon-run-standalone"};

			auto Arguments = fg_AppManager_BuildContainerRunArguments(Launch);
			TCVector<CStr> Expected =
				{
					"run"
					, "--name"
					, "mib-env-TestEnv"
					, "--interactive"
					, "--init"
					, "--user"
					, "0:0"
					, "--entrypoint"
					, "/opt/M/App/Agent/AppManager"
					, "ubuntu:24.04"
					, "--daemon-run-standalone"
				}
			;
			DMibExpect(Arguments, ==, Expected);
		}

		{
			DMibTestPath("Full");

			CAppManagerContainerLaunch Launch;
			Launch.m_ContainerName = "mib-env-TestEnv";
			Launch.m_Image = "ubuntu:24.04";
			Launch.m_bReadOnly = true;
			Launch.m_Network = "bridge";
			Launch.m_MemoryLimitMB = 512;
			Launch.m_CPULimit = 2.0;
			Launch.m_Mounts["/opt/M"] = "/opt/M";
			Launch.m_AddHosts["myhost"] = "host-gateway";
			Launch.m_PassEnvironment = {"MalterlibTestPassVariable"};
			Launch.m_WorkingDirectory = "/opt/M/Environment/TestEnv";
			Launch.m_ExtraArguments = {"--pull", "never"};
			Launch.m_Executable = "/opt/M/App/Agent/AppManager";
			Launch.m_Parameters = {"--daemon-run-standalone"};

			auto Arguments = fg_AppManager_BuildContainerRunArguments(Launch);
			TCVector<CStr> Expected =
				{
					"run"
					, "--name"
					, "mib-env-TestEnv"
					, "--interactive"
					, "--init"
					, "--user"
					, "0:0"
					, "--read-only"
					, "--tmpfs"
					, "/tmp"
					, "--network"
					, "bridge"
					, "--memory"
					, "512m"
					, "--cpus"
					, "2"
					, "--volume"
					, "/opt/M:/opt/M"
					, "--add-host"
					, "myhost:host-gateway"
					, "--env"
					, "MalterlibTestPassVariable"
					, "--workdir"
					, "/opt/M/Environment/TestEnv"
					, "--pull"
					, "never"
					, "--entrypoint"
					, "/opt/M/App/Agent/AppManager"
					, "ubuntu:24.04"
					, "--daemon-run-standalone"
				}
			;
			DMibExpect(Arguments, ==, Expected);
		}

		{
			DMibTestPath("Read-Only Mounts");

			CAppManagerContainerLaunch Launch;
			Launch.m_ContainerName = "mib-env-TestEnv";
			Launch.m_Image = "ubuntu:24.04";
			Launch.m_Mounts["/opt/M/Environment/TestEnv/Data/Root"] = "/opt/Malterlib/Data";
			Launch.m_ReadOnlyMounts["/opt/M/Environment/TestEnv/Agent"] = "/opt/Malterlib/Agent";
			Launch.m_Executable = "/opt/Malterlib/Agent/Current/AppManager";

			TCVector<CStr> ExpectedPrefix =
				{
					"run"
					, "--name"
					, "mib-env-TestEnv"
					, "--interactive"
					, "--init"
					, "--user"
					, "0:0"
					, "--volume"
					, "/opt/M/Environment/TestEnv/Data/Root:/opt/Malterlib/Data"
				}
			;

			TCVector<CStr> ExpectedSuffix =
				{
					"--entrypoint"
					, "/opt/Malterlib/Agent/Current/AppManager"
					, "ubuntu:24.04"
				}
			;

			{
				DMibTestPath("Docker");

				TCVector<CStr> Expected = ExpectedPrefix;
				Expected.f_Insert("--volume");
				Expected.f_Insert("/opt/M/Environment/TestEnv/Agent:/opt/Malterlib/Agent:ro");
				Expected.f_Insert(ExpectedSuffix);

				DMibExpect(fg_AppManager_BuildContainerRunArguments(Launch), ==, Expected);
			}

			{
				DMibTestPath("Apple Container");

				Launch.m_bAppleContainer = true;

				TCVector<CStr> Expected = ExpectedPrefix;
				Expected.f_Insert("--mount");
				Expected.f_Insert("type=bind,source=/opt/M/Environment/TestEnv/Agent,target=/opt/Malterlib/Agent,readonly");
				Expected.f_Insert(ExpectedSuffix);

				DMibExpect(fg_AppManager_BuildContainerRunArguments(Launch), ==, Expected);
			}
		}

		{
			DMibTestPath("Missing Container Errors");

			DMibExpectTrue(fg_AppManager_ContainerErrorReportsMissing("docker", "Error response from daemon: No such container: mib-env-TestEnv"));
			DMibExpectTrue(fg_AppManager_ContainerErrorReportsMissing("container", "Error: notFound: \"container not found: mib-env-TestEnv\""));
			DMibExpectFalse
				(
					fg_AppManager_ContainerErrorReportsMissing
					(
						"docker"
						, "Failed to initialize: unable to resolve docker endpoint: context \"missing\": context not found: open meta.json: no such file or directory"
					)
				)
			;
			DMibExpectFalse(fg_AppManager_ContainerErrorReportsMissing("container", "Error: internalError: \"failed to stop container\""));
		}

		{
			DMibTestPath("Missing Volume Errors");

			DMibExpectTrue(fg_AppManager_ContainerVolumeErrorReportsMissing("docker", "Error response from daemon: get mib-env-TestEnv-data: no such volume"));
			DMibExpectTrue(fg_AppManager_ContainerVolumeErrorReportsMissing("docker", "Error: No such volume: mib-env-TestEnv-data"));
			DMibExpectTrue(fg_AppManager_ContainerVolumeErrorReportsMissing("container", "Error: notFound: \"volume not found: mib-env-TestEnv-data\""));
			DMibExpectTrue(fg_AppManager_ContainerVolumeErrorReportsMissing("container", "Error: volume not found: mib-env-TestEnv-data"));
			DMibExpectFalse(fg_AppManager_ContainerVolumeErrorReportsMissing("container", "Error: failed to delete one or more volumes: [\"mib-env-TestEnv-data\"]"));
			DMibExpectFalse(fg_AppManager_ContainerVolumeErrorReportsMissing("docker", "Error response from daemon: remove mib-env-TestEnv-data: volume is in use - [0123456789abcdef]"));
			DMibExpectFalse(fg_AppManager_ContainerVolumeErrorReportsMissing("docker", "Cannot connect to the Docker daemon at unix:///var/run/docker.sock. Is the docker daemon running?"));
		}

		co_return {};
	};

	DMibTestSuite("Guest Deferral")
	{
		fp64 Start = CAppManagerGuestDeferral::fs_Now();
		auto fAt = [&](fp64 _Seconds)
			{
				return Start + _Seconds;
			}
		;
		constexpr fp64 c_Limit = CAppManagerGuestDeferral::mc_LimitSeconds;

		{
			DMibTestPath("Limited");

			CAppManagerGuestDeferral Deferral;
			DMibExpectFalse(Deferral.f_Defers(fAt(0)));

			Deferral.f_Update(true, fAt(0));
			DMibExpect(Deferral.f_End(), ==, fAt(c_Limit));
			DMibExpectTrue(Deferral.f_Defers(fAt(c_Limit - 1)));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit)));

			Deferral.f_Update(true, fAt(c_Limit + 10));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit + 10)));
		}

		{
			DMibTestPath("Toggling Continues");

			CAppManagerGuestDeferral Deferral;
			Deferral.f_Update(true, fAt(0));
			Deferral.f_Update(false, fAt(c_Limit - 10));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit - 10)));

			Deferral.f_Update(true, fAt(c_Limit - 5));
			DMibExpect(Deferral.f_End(), ==, fAt(c_Limit));
			DMibExpectTrue(Deferral.f_Defers(fAt(c_Limit - 5)));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit + 5)));

			Deferral.f_Update(false, fAt(c_Limit + 20));
			Deferral.f_Update(true, fAt(c_Limit + 30));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit + 30)));
		}

		{
			DMibTestPath("Quiet Period Resets");

			CAppManagerGuestDeferral Deferral;
			Deferral.f_Update(true, fAt(0));
			Deferral.f_Update(false, fAt(10));
			Deferral.f_Update(true, fAt(c_Limit + 10));
			DMibExpectTrue(Deferral.f_Defers(fAt(c_Limit + 10)));
			DMibExpectTrue(Deferral.f_Defers(fAt(c_Limit * 2 + 9)));
			DMibExpectFalse(Deferral.f_Defers(fAt(c_Limit * 2 + 10)));
		}
	};

	DMibTestSuite("Hosts File")
	{
		{
			DMibTestPath("Alias");
			DMibExpectTrue(fg_AppManager_HostsFileHasHostName("127.0.0.1\tlocalhost\n127.0.1.1 Host-Env host-env.local\n", "host-env"));
		}

		{
			DMibTestPath("Empty");
			DMibExpectFalse(fg_AppManager_HostsFileHasHostName("", "host-env"));
		}

		{
			DMibTestPath("Longer Name");
			DMibExpectFalse(fg_AppManager_HostsFileHasHostName("127.0.0.1 localhost host-env2\n", "host-env"));
		}

		{
			DMibTestPath("Address Only");
			DMibExpectFalse(fg_AppManager_HostsFileHasHostName("host-env localhost\n", "host-env"));
		}

		{
			DMibTestPath("Comment");
			DMibExpectFalse(fg_AppManager_HostsFileHasHostName("# host-env\n127.0.0.1 localhost #host-env\n", "host-env"));
		}

		{
			DMibTestPath("Name Before Comment");
			DMibExpectTrue(fg_AppManager_HostsFileHasHostName("127.0.0.1 host-env# Environment\n", "host-env"));
		}
	};

	DMibTestSuite("Environment Host Name")
	{
		{
			DMibTestPath("Plain");
			DMibExpect(fg_AppManager_EnvironmentHostName("Host", "Env"), ==, "Host-Env");
		}

		{
			DMibTestPath("Sanitized Names Stay Apart");

			CStr Dotted = fg_AppManager_EnvironmentHostName("Host", "api.blue");
			DMibExpectTrue(Dotted.f_StartsWith("Host-api-blue-"));
			DMibExpect(Dotted, !=, fg_AppManager_EnvironmentHostName("Host", "api-blue"));
		}

		{
			DMibTestPath("Long Computer Name");

			// The computer name is shortened to 46 characters to make room for "-Env-<Hash>", which would end it with its '-'
			CStr Computer;
			for (umint iCharacter = 0; iCharacter < 45; ++iCharacter)
				Computer += "a";
			Computer += "-bcdefghijklmnop";

			CStr HostName = fg_AppManager_EnvironmentHostName(Computer, "Env");
			DMibExpect(HostName.f_GetLen(), <=, 63u);
			DMibExpectTrue(HostName.f_StartsWith(Computer.f_Left(45) + "-Env-"));
			DMibExpectFalse(HostName.f_StartsWith("-"));
			DMibExpectFalse(HostName.f_Find("--") >= 0);
		}

		{
			DMibTestPath("Shortened Computer Name Stays Apart");

			// Without the suffix both are development-node-eu-west-01-analytics-warehouse-us-east-prod-01
			CStr Computer = "development-node-eu-west-01";
			CStr Short = fg_AppManager_EnvironmentHostName(Computer, "analytics-warehouse-us-east-prod-01");
			CStr Long = fg_AppManager_EnvironmentHostName(Computer, "eu-west-01-analytics-warehouse-us-east-prod-01");
			DMibExpect(Short, ==, "development-node-eu-west-01-analytics-warehouse-us-east-prod-01");
			DMibExpect(Short, !=, Long);
			DMibExpect(Long.f_GetLen(), <=, 63u);
		}

		{
			DMibTestPath("Hosts With Shortened Computer Names Stay Apart");

			// Both computer names are cut to production-worker
			CStr First = fg_AppManager_EnvironmentHostName("production-worker-us-east-001", "analytics-warehouse-us-east-prod-01");
			CStr Second = fg_AppManager_EnvironmentHostName("production-worker-us-east-002", "analytics-warehouse-us-east-prod-01");
			DMibExpectTrue(First.f_StartsWith("production-worker-analytics-"));
			DMibExpect(First, !=, Second);
			DMibExpect(First.f_GetLen(), <=, 63u);
		}

		{
			DMibTestPath("Long Environment Names Stay Apart");

			// Valid environment names have dotted labels of up to 63 characters each
			CStr Start;
			for (umint iCharacter = 0; iCharacter < 63; ++iCharacter)
				Start += "a";

			CStr First = fg_AppManager_EnvironmentHostName("Host", Start + ".first");
			CStr Second = fg_AppManager_EnvironmentHostName("Host", Start + ".second");
			DMibExpect(First, !=, Second);
			DMibExpect(First.f_GetLen(), <=, 63u);
			DMibExpect(Second.f_GetLen(), <=, 63u);
			DMibExpectTrue(First.f_StartsWith("Host-"));

			// The suffixes of these names collide in a DJB2 hash
			DMibExpect(fg_AppManager_EnvironmentHostName("Host", Start + ".Aq"), !=, fg_AppManager_EnvironmentHostName("Host", Start + ".BP"));

			CStr Undotted = fg_AppManager_EnvironmentHostName("Host", Start);
			DMibExpect(Undotted.f_GetLen(), <=, 63u);
			DMibExpectTrue(Undotted.f_StartsWith("Host-"));
		}

		{
			DMibTestPath("Shared Host Name");
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--uts=host"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--uts", "host"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({}, "container:Other"));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network", "container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--net=container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=name=container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({}, "name=container:Other"));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=NAME=container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=alias=a, name = container:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=\"name=container:Other\""}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=name=CONTAINER:Other"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({}, "Name=Container:Other"));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({}, "CONTAINER:Other"));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({"--network=name=bridge,alias=container:Other"}, ""));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({"--network=bridge"}, ""));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({"--uts", "private"}, "bridge"));

			// The last option and the last name field apply
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({"--uts=host", "--uts="}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--uts=private", "--uts", "host"}, ""));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({"--network=name=container:Other,name=bridge"}, ""));
			DMibExpectTrue(fg_AppManager_ContainerSharesHostName({"--network=name=bridge,name=container:Other"}, ""));
			DMibExpectFalse(fg_AppManager_ContainerSharesHostName({}, "name=container:Other,name=bridge"));
		}
	};
}

DMibTestRegister(CAppManager_Environment_Tests, Malterlib::Cloud);
