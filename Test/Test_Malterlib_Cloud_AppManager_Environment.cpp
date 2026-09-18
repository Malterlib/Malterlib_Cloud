// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include "Test_Malterlib_Cloud_AppManager.h"

#ifdef DPlatformFamily_Windows
#	include <Windows.h>
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
			DMibTestPath("Parent Application Missing");

			CAppManagerInterface::CEnvironmentSettings Settings;
			Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
			Settings.m_ParentApplication = CStr("NoSuchApp");

			auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ConfinedEnv", Settings).f_Timeout(g_Timeout, "Timed out adding environment").f_Wrap();
			DMibExpectFalse(Result);
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
			DMibExpect(Environments.f_GetLen(), ==, 0u);
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

			co_await Interface.f_CallActor(&CAppManagerInterface::f_ChangeSettings)("TestApp", ChangeSettings, Settings).f_Timeout(g_Timeout, "Timed out changing application settings");

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
#endif

#ifndef DPlatformFamily_Windows
	DMibTestSuite("Environment Socket Directory")
	{
		{
			DMibTestPath("Root Directory");

			auto Directory = fg_AppManager_EnvironmentSocketDirectory("/opt/AppManager", "Env", false);
			DMibExpect(Directory.m_SharedDirectory, ==, "/opt/AppManager/Sockets");
			DMibExpect(CFile::fs_GetPath(Directory.m_SocketDirectory), ==, Directory.m_SharedDirectory);
			DMibExpect(Directory.m_SocketPath, ==, Directory.m_SocketDirectory / "Agent.socket");

			auto Other = fg_AppManager_EnvironmentSocketDirectory("/opt/AppManager", "OtherEnv", false);
			DMibExpect(Other.m_SocketDirectory, !=, Directory.m_SocketDirectory);
		}

		{
			DMibTestPath("Long Root Directory");

			CStr RootDirectory = "/opt";
			for (umint iComponent = 0; iComponent < 10; ++iComponent)
				RootDirectory = RootDirectory / "LongDirectoryName";

			auto AsRoot = fg_AppManager_EnvironmentSocketDirectory(RootDirectory, "Env", true);
			DMibExpect(AsRoot.m_SharedDirectory, ==, "/var/run/MalterlibAppManager");
			DMibExpect(CFile::fs_GetPath(AsRoot.m_SocketDirectory), ==, AsRoot.m_SharedDirectory);

			auto AsOtherUser = fg_AppManager_EnvironmentSocketDirectory(RootDirectory, "Env", false);
			DMibExpect(AsOtherUser.m_SharedDirectory, ==, "");
			DMibExpect(CFile::fs_GetPath(AsOtherUser.m_SocketDirectory), ==, CFile::fs_GetRawTemporaryDirectory());
			DMibExpect(AsOtherUser.m_SocketDirectory, !=, fg_AppManager_EnvironmentSocketDirectory(RootDirectory + "B", "Env", false).m_SocketDirectory);
		}

		{
			DMibTestPath("Prepare");

			CStr RootDirectory = CFile::fs_GetProgramDirectory() / "AppManagerEnvironmentSocketDirectory";

			fg_TestAddCleanupPath(RootDirectory);

			if (CFile::fs_FileExists(RootDirectory))
				CFile::fs_DeleteDirectoryRecursive(RootDirectory);

			CAppManagerEnvironmentSocketDirectory Directory;
			Directory.m_SharedDirectory = RootDirectory / "Shared";
			Directory.m_SocketDirectory = Directory.m_SharedDirectory / "Env";

			constexpr EFileAttrib c_Traversable = EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute | EFileAttrib_GroupExecute | EFileAttrib_EveryoneExecute;

			fg_AppManager_PrepareEnvironmentSocketDirectory(Directory);
			DMibExpect(CFile::fs_GetAttributes(Directory.m_SharedDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Traversable);
			DMibExpect(CFile::fs_GetAttributes(Directory.m_SocketDirectory) & EFileAttrib_AllUnixPermissions, ==, c_Traversable);
		}
	};
#endif

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
}

DMibTestRegister(CAppManager_Environment_Tests, Malterlib::Cloud);
