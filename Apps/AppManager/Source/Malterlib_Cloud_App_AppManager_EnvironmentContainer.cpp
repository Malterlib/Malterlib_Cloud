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
		Arguments.f_Insert("--interactive");

		if (_Launch.m_bReadOnly)
			Arguments.f_Insert("--read-only");

		if (!_Launch.m_Network.f_IsEmpty())
		{
			Arguments.f_Insert("--network");
			Arguments.f_Insert(_Launch.m_Network);
		}

		if (_Launch.m_MemoryLimitMB != 0)
		{
			Arguments.f_Insert("--memory");
			Arguments.f_Insert("{}m"_f << _Launch.m_MemoryLimitMB);
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

		if (Runtime.f_IsEmpty())
		{
#ifdef DPlatformFamily_macOS
			return "container";
#else
			return "docker";
#endif
		}

		if (Runtime == "Docker")
			return "docker";

		if (Runtime == "AppleContainer")
			return "container";

		return Runtime;
	}

	CStr CAppManagerActor::fp_GetContainerName(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		return "mib-env-{}"_f << _pEnvironment->m_Name;
	}

	bool CAppManagerActor::fp_UseOwnAppleContainerSystem()
	{
#ifdef DPlatformFamily_macOS
		return CProcessLaunch::fs_GetElevation() == NProcess::EProcessElevation_IsRoot;
#else
		return false;
#endif
	}

	CStr CAppManagerActor::fp_GetAppleContainerAppRoot()
	{
		return mp_State.m_RootDirectory / "AppleContainer";
	}

	void CAppManagerActor::fp_AdjustAppleContainerCommand(CStr &_Executable, TCVector<CStr> &_Arguments)
	{
		if (_Executable != "container" || !fp_UseOwnAppleContainerSystem())
			return;

		// The Apple container API server only accepts clients with its own effective user
		// id and is looked up through the caller's launchd namespace. Executing the client
		// in the bootstrap namespace of pid 1 keeps the lookup in the system domain, away
		// from any login session's per-user service, so a root AppManager always reaches
		// the AppManager-owned service that `container system start` registers there
		TCVector<CStr> Arguments = fg_CreateVector<CStr>("bsexec", "1", "/usr/bin/env", "container");
		for (auto &Argument : _Arguments)
			Arguments.f_Insert(fg_Move(Argument));

		_Executable = "/bin/launchctl";
		_Arguments = fg_Move(Arguments);
	}

	void CAppManagerActor::fp_ApplyAppleContainerLaunchEnvironment(CProcessLaunchParams &_LaunchParams)
	{
		// The client and the AppManager-owned API server must agree on the data location
		_LaunchParams.m_Environment["CONTAINER_APP_ROOT"] = fp_GetAppleContainerAppRoot();

		// The AppManager can run as a daemon whose PATH misses the CLI install location
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;
		_LaunchParams.m_Environment["PATH"] = Path;
	}

	auto CAppManagerActor::fp_BuildContainerCommandParams
		(
			TCSharedPointer<CEnvironment> const &_pEnvironment
			, TCVector<CStr> &&_Arguments
		)
		-> CProcessLaunchParams
	{
		CStr Executable = fp_GetContainerRuntimeExecutable(_pEnvironment);
		TCVector<CStr> Arguments = fg_Move(_Arguments);

		bool bOwnAppleContainerSystem = Executable == "container" && fp_UseOwnAppleContainerSystem();
		fp_AdjustAppleContainerCommand(Executable, Arguments);

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				Executable
				, fg_Move(Arguments)
				, mp_State.m_RootDirectory
				, {}
			)
		;
		LaunchParams.m_bAllowExecutableLocate = true;
		LaunchParams.m_bMergeEnvironment = true;

		if (bOwnAppleContainerSystem)
			fp_ApplyAppleContainerLaunchEnvironment(LaunchParams);

		return LaunchParams;
	}

	TCFuture<void> CAppManagerActor::fp_EnsureAppleContainerSystem(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) != "container" || !fp_UseOwnAppleContainerSystem())
			co_return {};

		if (mp_bAppleContainerSystemReady)
			co_return {};

		if (mp_bAppleContainerSystemStarting)
			co_return co_await mp_OnAppleContainerSystemReady.f_Insert().f_Future();

		mp_bAppleContainerSystemStarting = true;

		bool bReady = false;
		auto Cleanup = g_OnScopeExit / [&, this]
			{
				mp_bAppleContainerSystemStarting = false;
				mp_bAppleContainerSystemReady = bReady;
				auto Waiters = fg_Move(mp_OnAppleContainerSystemReady);
				for (auto &Promise : Waiters)
				{
					if (bReady)
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance("Failed to start the AppManager container system"));
				}
			}
		;

		CStr AppRoot = fp_GetAppleContainerAppRoot();

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [AppRoot]()
						{
							CFile::fs_CreateDirectory(AppRoot);
						}
					)
					% "Failed to create the container system directory"
				)
			;
		}

		// The container system keeps running after the AppManager exits, like the
		// containers it hosts; starting it while it is running is a no-op health check.
		// The first start also downloads the default kernel and the init filesystem
		CProcessLaunchParams LaunchParams = fp_BuildContainerCommandParams
			(
				_pEnvironment
				, fg_CreateVector<CStr>("system", "start", "--app-root", AppRoot, "--enable-kernel-install")
			)
		;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode)
			)
			.f_Wrap()
		;

		if (!Result)
			co_return DMibErrorInstance("Failed to start the AppManager container system: {}"_f << Result.f_GetExceptionStr());

		bReady = true;

		co_return {};
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
		Launch.m_MemoryLimitMB = Settings.m_MemoryLimitMB;
		Launch.m_CPULimit = Settings.m_CPULimit;
		Launch.m_bReadOnly = Settings.m_bContainerReadOnly;
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

		// Environments confined to a parent application store their data inside that
		// application's directory, which can be a separate (encrypted) mount. Nested
		// mounts are not visible through the root bind mount, so it is mounted explicitly
		if (!Settings.m_ParentApplication.f_IsEmpty())
			Launch.m_Mounts[_AgentRootDirectory] = _AgentRootDirectory;

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
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchParams LaunchParams = fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("rm", "--force", ContainerName));

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

	TCFuture<void> CAppManagerActor::fp_StopEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchParams LaunchParams = fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("stop", ContainerName));

		co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None)
			)
			.f_Wrap()
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment container")
		;

		co_return {};
	}

	TCFuture<bool> CAppManagerActor::fp_EnvironmentContainerExists(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchParams LaunchParams = fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("inspect", ContainerName));

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None)
			)
			.f_Wrap()
		;

		co_return Result && Result->m_ExitCode == 0;
	}

	TCFuture<void> CAppManagerActor::fp_PullEnvironmentContainerImage(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		TCVector<CStr> Arguments;
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) == "container")
			Arguments = fg_CreateVector<CStr>("image", "pull", _pEnvironment->m_Settings.m_ContainerImage);
		else
			Arguments = fg_CreateVector<CStr>("pull", _pEnvironment->m_Settings.m_ContainerImage);

		CProcessLaunchParams LaunchParams = fp_BuildContainerCommandParams(_pEnvironment, fg_Move(Arguments));

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
			(
				CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode)
			)
			.f_Wrap()
		;

		if (!Result)
			co_return DMibErrorInstance("Failed to pull image '{}': {}"_f << _pEnvironment->m_Settings.m_ContainerImage << Result.f_GetExceptionStr());

		co_return {};
	}
}
