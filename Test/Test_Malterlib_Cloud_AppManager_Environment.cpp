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
}

DMibTestRegister(CAppManager_Environment_Tests, Malterlib::Cloud);
