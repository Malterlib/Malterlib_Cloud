// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	NContainer::TCVector<NStr::CStr> fg_AppManager_BuildContainerRunArguments(CAppManagerContainerLaunch const &_Launch)
	{
		using namespace NMib::NStr;

		TCVector<CStr> Arguments;

		Arguments.f_Insert("run");
		Arguments.f_Insert("--name");
		Arguments.f_Insert(_Launch.m_ContainerName);
		Arguments.f_Insert("--rm");
		Arguments.f_Insert("--interactive");

		if (!_Launch.m_Network.f_IsEmpty())
		{
			Arguments.f_Insert("--network");
			Arguments.f_Insert(_Launch.m_Network);
		}

		if (!_Launch.m_MemoryLimit.f_IsEmpty())
		{
			Arguments.f_Insert("--memory");
			Arguments.f_Insert(_Launch.m_MemoryLimit);
		}

		if (_Launch.m_CPULimit != 0.0)
		{
			Arguments.f_Insert("--cpus");
			Arguments.f_Insert(CStr::fs_ToStr(_Launch.m_CPULimit));
		}

		for (auto &Mount : _Launch.m_Mounts)
		{
			Arguments.f_Insert("--volume");
			Arguments.f_Insert("{}:{}"_f << _Launch.m_Mounts.fs_GetKey(Mount) << Mount);
		}

		for (auto &AddHost : _Launch.m_AddHosts)
		{
			Arguments.f_Insert("--add-host");
			Arguments.f_Insert("{}:{}"_f << _Launch.m_AddHosts.fs_GetKey(AddHost) << AddHost);
		}

		for (auto &Variable : _Launch.m_PassEnvironment)
		{
			Arguments.f_Insert("--env");
			Arguments.f_Insert(Variable);
		}

		if (!_Launch.m_WorkingDirectory.f_IsEmpty())
		{
			Arguments.f_Insert("--workdir");
			Arguments.f_Insert(_Launch.m_WorkingDirectory);
		}

		for (auto &Argument : _Launch.m_ExtraArguments)
			Arguments.f_Insert(Argument);

		Arguments.f_Insert(_Launch.m_Image);
		Arguments.f_Insert(_Launch.m_Executable);

		for (auto &Parameter : _Launch.m_Parameters)
			Arguments.f_Insert(Parameter);

		return Arguments;
	}
}

namespace NMib::NCloud::NAppManager
{
	CStr CAppManagerActor::fp_GetContainerRuntimeExecutable(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		auto &Runtime = _pEnvironment->m_Settings.m_ContainerRuntime;

		if (Runtime == "Docker" || Runtime.f_IsEmpty())
			return "docker";

		if (Runtime == "AppleContainer")
			return "container";

		return Runtime;
	}

	CStr CAppManagerActor::fp_GetContainerName(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		return "mib-env-{}"_f << _pEnvironment->m_Name;
	}

	auto CAppManagerActor::fp_BuildEnvironmentContainerLaunch
		(
			TCSharedPointer<CEnvironment> const &_pEnvironment
			, CStr const &_AgentExecutable
			, CStr const &_AgentRootDirectory
		)
		-> CAppManagerContainerLaunch
	{
		auto &Settings = _pEnvironment->m_Settings;

		CAppManagerContainerLaunch Launch;
		Launch.m_ContainerName = fp_GetContainerName(_pEnvironment);
		Launch.m_Image = Settings.m_ContainerImage;
		Launch.m_MemoryLimit = Settings.m_MemoryLimit;
		Launch.m_CPULimit = Settings.m_CPULimit;
		Launch.m_ExtraArguments = Settings.m_ContainerExtraArguments;
		Launch.m_WorkingDirectory = _AgentRootDirectory;
		Launch.m_Executable = _AgentExecutable;
		Launch.m_Parameters = {"--daemon-run-standalone"};

		if (!Settings.m_ContainerNetwork.f_IsEmpty())
			Launch.m_Network = Settings.m_ContainerNetwork;
#ifdef DPlatformFamily_Linux
		else
			Launch.m_Network = "host";
#endif

		// The root directory is mounted at the same path inside the container so that
		// application directories and local socket addresses stay valid inside it
		Launch.m_Mounts[mp_State.m_RootDirectory] = mp_State.m_RootDirectory;
		for (auto &Mount : Settings.m_ContainerExtraMounts)
			Launch.m_Mounts[Settings.m_ContainerExtraMounts.fs_GetKey(Mount)] = Mount;

		// Make the host reachable by the name used in the local address when the
		// container is not sharing the host network. The Apple container runtime has
		// no --add-host option; on its vmnet network the host is reached through DNS.
		if (Launch.m_Network != "host" && fp_GetContainerRuntimeExecutable(_pEnvironment) != "container")
		{
			CStr Host = mp_State.m_LocalAddress.f_GetHost();
			if (!Host.f_IsEmpty() && !Host.f_StartsWith("UNIX("))
				Launch.m_AddHosts[Host] = "host-gateway";
		}

		// Environment variables forwarded from the launching process into the container
		Launch.m_PassEnvironment =
			{
				"MalterlibDistributedAppInterfaceServerAddress"
				, "MalterlibDistributedAppInterfaceServerRequestTicket"
				, "MalterlibDistributedAppInterfaceServerLaunchID"
				, "MalterlibDistributedAppInterfaceServerOptions"
				, "MalterlibProtectedEnvironment"
				, "MalterlibAppManagerEnvironmentAgentRoot"
				, "MalterlibAppManagerEnvironmentHostID"
				, "HOME={}/.home"_f << _AgentRootDirectory
				, "TMPDIR={}/.tmp"_f << _AgentRootDirectory
			}
		;

		return Launch;
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr RuntimeExecutable = fp_GetContainerRuntimeExecutable(_pEnvironment);
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				RuntimeExecutable
				, fg_CreateVector<CStr>("rm", "--force", ContainerName)
				, mp_State.m_RootDirectory
				, {}
			)
		;
		LaunchParams.m_bAllowExecutableLocate = true;
		LaunchParams.m_bMergeEnvironment = true;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode)
			)
			.f_Wrap()
		;

		if (!Result)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Info
					, "Failed to remove container '{}': {}"
					, ContainerName
					, Result.f_GetExceptionStr()
				)
			;
		}

		co_return {};
	}
}
