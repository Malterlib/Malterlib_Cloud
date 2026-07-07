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
	void f_DoTests()
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
					co_return co_await Interface.f_CallActor(&CAppManagerInterface::f_GetEnvironments)()
						.f_Timeout(g_Timeout, "Timed out enumerating environments")
					;
				}
			;

			// No environments initially
			{
				DMibTestPath("Initial");

				auto Environments = co_await fGetEnvironments();
				DMibExpect(Environments.f_GetLen(), ==, 0u);
			}

			// Add a local environment
			{
				DMibTestPath("Add Local");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;
				Settings.m_bAutoStart = false;

				co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("TestEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out adding environment")
				;
			}

			// The environment is listed with its settings
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

			// Adding a duplicate environment fails
			{
				DMibTestPath("Duplicate Add");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_Type = CAppManagerInterface::EEnvironmentType_Local;

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("TestEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out adding environment")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			// Adding a container environment without an image fails
			{
				DMibTestPath("Container Without Image");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ContainerEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out adding environment")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			// Adding a container environment with an image succeeds
			{
				DMibTestPath("Add Container");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_Type = CAppManagerInterface::EEnvironmentType_Container;
				Settings.m_ContainerImage = CStr("ubuntu:24.04");
				Settings.m_AgentApplication = CStr("SelfUpdate.Linux-arm64");

				co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentAdd)("ContainerEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out adding environment")
				;
			}

			// Change settings
			{
				DMibTestPath("Change Settings");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_ContainerNetwork = CStr("host");
				Settings.m_MemoryLimit = CStr("512m");

				co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)("ContainerEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out changing environment settings")
				;

				auto Environments = co_await fGetEnvironments();
				auto pEnvironment = Environments.f_FindEqual("ContainerEnv");
				DMibExpectTrue(pEnvironment != nullptr);
				DMibExpect(pEnvironment->m_ContainerNetwork, ==, "host");
				DMibExpect(pEnvironment->m_MemoryLimit, ==, "512m");
				DMibExpect(pEnvironment->m_ContainerImage, ==, "ubuntu:24.04");
				DMibExpect(pEnvironment->m_AgentApplication, ==, "SelfUpdate.Linux-arm64");
			}

			// Changing settings to an invalid configuration fails
			{
				DMibTestPath("Invalid Change Settings");

				CAppManagerInterface::CEnvironmentSettings Settings;
				Settings.m_ContainerImage = CStr("");

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentChangeSettings)("ContainerEnv", Settings)
					.f_Timeout(g_Timeout, "Timed out changing environment settings")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			// Add a null application that references the environment
			{
				DMibTestPath("Referencing Application");

				CAppManagerInterface::CApplicationAdd Add;
				CAppManagerInterface::CApplicationSettings Settings;
				Settings.m_LaunchEnvironment = CStr("TestEnv");

				co_await Interface.f_CallActor(&CAppManagerInterface::f_Add)("EnvApp", Add, Settings)
					.f_Timeout(g_Timeout, "Timed out adding application")
				;

				auto Environments = co_await fGetEnvironments();
				auto pEnvironment = Environments.f_FindEqual("TestEnv");
				DMibExpectTrue(pEnvironment != nullptr);
				DMibExpect(pEnvironment->m_Applications.f_GetLen(), ==, 1u);
				DMibExpectTrue(pEnvironment->m_Applications.f_FindEqual("EnvApp") != nullptr);
			}

			// Removing an environment that is referenced by an application fails
			{
				DMibTestPath("Remove Referenced");

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv")
					.f_Timeout(g_Timeout, "Timed out removing environment")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			// Starting environments is not yet supported
			{
				DMibTestPath("Start Unsupported");

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentStart)("TestEnv")
					.f_Timeout(g_Timeout, "Timed out starting environment")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			// Remove the application, then the environment can be removed
			{
				DMibTestPath("Remove");

				co_await Interface.f_CallActor(&CAppManagerInterface::f_Remove)("EnvApp")
					.f_Timeout(g_Timeout, "Timed out removing application")
				;

				co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv")
					.f_Timeout(g_Timeout, "Timed out removing environment")
				;

				auto Environments = co_await fGetEnvironments();
				DMibExpect(Environments.f_GetLen(), ==, 1u);
				DMibExpectTrue(Environments.f_FindEqual("TestEnv") == nullptr);
			}

			// Removing a non-existing environment fails
			{
				DMibTestPath("Remove Missing");

				auto Result = co_await Interface.f_CallActor(&CAppManagerInterface::f_EnvironmentRemove)("TestEnv")
					.f_Timeout(g_Timeout, "Timed out removing environment")
					.f_Wrap()
				;
				DMibExpectFalse(Result);
			}

			co_return {};
		};
	}
};

DMibTestRegister(CAppManager_Environment_Tests, Malterlib::Cloud);
