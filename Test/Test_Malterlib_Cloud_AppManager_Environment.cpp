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
