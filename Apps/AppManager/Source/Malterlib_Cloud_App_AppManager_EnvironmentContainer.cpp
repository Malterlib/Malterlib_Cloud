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

	void CAppManagerActor::fp_ApplyAppleContainerLaunchEnvironment(CProcessLaunchParams &_LaunchParams)
	{
		// The client and the AppManager-owned API server must agree on the data location.
		// The mach service lookup falls through the login session and per-user launchd
		// domains to the system domain, so the client reaches the AppManager-owned
		// service as long as no login session runs its own container system; a session
		// one cannot coexist anyway because the API server binds fixed localhost ports
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

		bool bOwnAppleContainerSystem = Executable == "container" && fp_UseOwnAppleContainerSystem();

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				Executable
				, fg_Move(_Arguments)
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
		CStr KernelMarkerFile = AppRoot / ".MalterlibKernelInstalled";

		struct CPrepareResult
		{
			CStr m_Error;
			CStr m_PlistFile;
			bool m_bKernelInstalled = false;
		};

		CPrepareResult Prepared;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Prepared = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [AppRoot, KernelMarkerFile]() -> CPrepareResult
					{
						CPrepareResult Result;

						// Find the Apple container CLI and resolve it to the real binary; the
						// API server executable lives next to it and launchd must reference it
						// by its true path for the code signature validation to pass
						CStr Executable;
						for (auto &Candidate : {CStr("/usr/local/bin/container"), CStr("/opt/homebrew/bin/container")})
						{
							if (CFile::fs_FileExists(Candidate))
							{
								Executable = Candidate;
								break;
							}
						}
						if (Executable.f_IsEmpty())
							Executable = NProcess::NPlatform::fg_FindExecutable("container");
						if (Executable.f_IsEmpty())
						{
							Result.m_Error = "The Apple container CLI was not found";
							return Result;
						}

						for (aint Depth = 0; Depth < 16 && CFile::fs_FileExists(Executable, EFileAttrib_Link); ++Depth)
						{
							CStr Target = CFile::fs_ResolveSymbolicLink(Executable);
							if (!Target.f_StartsWith("/"))
								Target = CFile::fs_GetPath(Executable) / Target;
							Executable = Target;
						}

						CStr BinaryDirectory = CFile::fs_GetPath(Executable);
						CStr ApiServer = BinaryDirectory / "container-apiserver";
						if (!CFile::fs_FileExists(ApiServer))
						{
							Result.m_Error = "Found no container-apiserver next to '{}'"_f << Executable;
							return Result;
						}

						CStr InstallRoot = CFile::fs_GetPath(BinaryDirectory);

						auto fEscapeXml = [](CStr const &_Text)
							{
								CStr Escaped = _Text;
								Escaped.f_Replace("&", "&amp;");
								Escaped.f_Replace("<", "&lt;");
								return Escaped;
							}
						;

						// The same launchd job `container system start` would register, but
						// written and bootstrapped by the AppManager so the registration does
						// not depend on the session-derived domain of the calling process
						CStr Plist = fg_Format
							(
								"<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
								"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
								"<plist version=\"1.0\">\n"
								"<dict>\n"
								"\t<key>Label</key>\n"
								"\t<string>com.apple.container.apiserver</string>\n"
								"\t<key>ProgramArguments</key>\n"
								"\t<array>\n"
								"\t\t<string>{}</string>\n"
								"\t\t<string>start</string>\n"
								"\t</array>\n"
								"\t<key>EnvironmentVariables</key>\n"
								"\t<dict>\n"
								"\t\t<key>CONTAINER_APP_ROOT</key>\n"
								"\t\t<string>{}</string>\n"
								"\t\t<key>CONTAINER_INSTALL_ROOT</key>\n"
								"\t\t<string>{}</string>\n"
								"\t\t<key>CONTAINER_LOG_ROOT</key>\n"
								"\t\t<string>{}</string>\n"
								"\t</dict>\n"
								"\t<key>MachServices</key>\n"
								"\t<dict>\n"
								"\t\t<key>com.apple.container.apiserver</key>\n"
								"\t\t<true/>\n"
								"\t</dict>\n"
								"\t<key>RunAtLoad</key>\n"
								"\t<true/>\n"
								"\t<key>LimitLoadToSessionType</key>\n"
								"\t<array>\n"
								"\t\t<string>Aqua</string>\n"
								"\t\t<string>Background</string>\n"
								"\t\t<string>System</string>\n"
								"\t</array>\n"
								"</dict>\n"
								"</plist>\n"
								, fEscapeXml(ApiServer)
								, fEscapeXml(AppRoot)
								, fEscapeXml(InstallRoot)
								, fEscapeXml(AppRoot / "logs")
							)
						;

						CFile::fs_CreateDirectory(AppRoot);
						CFile::fs_CreateDirectory(AppRoot / "apiserver");
						CFile::fs_CreateDirectory(AppRoot / "logs");

						Result.m_PlistFile = AppRoot / "apiserver" / "apiserver.plist";
						CFile::fs_WriteStringToFile(Result.m_PlistFile, Plist);

						Result.m_bKernelInstalled = CFile::fs_FileExists(KernelMarkerFile);

						return Result;
					}
				)
			;
		}

		if (!Prepared.m_Error.f_IsEmpty())
			co_return DMibErrorInstance("Failed to start the AppManager container system: {}"_f << Prepared.m_Error);

		auto fLaunchctl = [this](TCVector<CStr> &&_Arguments)
			{
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
					(
						"/bin/launchctl"
						, fg_Move(_Arguments)
						, mp_State.m_RootDirectory
						, {}
					)
				;
				LaunchParams.m_bMergeEnvironment = true;

				return CProcessLaunchActor::fs_LaunchSimple
					(
						CProcessLaunchActor::CSimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None)
					)
				;
			}
		;

		// Registration is idempotent: bootstrapping an already loaded label fails and the
		// health check below decides. The API service keeps running after the AppManager
		// exits, like the containers it hosts
		auto BootstrapResult = co_await fLaunchctl(fg_CreateVector<CStr>("bootstrap", "system", Prepared.m_PlistFile)).f_Wrap();

		CStr LastError;
		bool bHealthy = false;
		for (aint Attempt = 0; !bHealthy && Attempt < 6; ++Attempt)
		{
			if (Attempt == 3)
			{
				// The service may be registered with a stale configuration; replace it
				auto BootOutResult = co_await fLaunchctl(fg_CreateVector<CStr>("bootout", "system/com.apple.container.apiserver")).f_Wrap();
				BootstrapResult = co_await fLaunchctl(fg_CreateVector<CStr>("bootstrap", "system", Prepared.m_PlistFile)).f_Wrap();
			}
			else if (Attempt != 0)
				co_await fg_Timeout(2.0);

			auto Health = co_await CProcessLaunchActor::fs_LaunchSimple
				(
					CProcessLaunchActor::CSimpleLaunch
						(
							fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("list"))
							, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
						)
				)
				.f_Wrap()
			;

			if (Health)
				bHealthy = true;
			else
				LastError = Health.f_GetExceptionStr();
		}

		if (!bHealthy)
		{
			CStr LogFile = AppRoot / "logs" / "container-apiserver.log";

			CStr LogTail;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				LogTail = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [LogFile]() -> CStr
						{
							try
							{
								if (!CFile::fs_FileExists(LogFile))
									return {};

								constexpr umint c_MaxTailLen = 4096;

								CStr Contents = CFile::fs_ReadStringFromFile(LogFile);
								if (Contents.f_GetLen() <= c_MaxTailLen)
									return Contents;

								CStr Tail = Contents.f_Extract(Contents.f_GetLen() - c_MaxTailLen);

								// Start the tail at a line boundary
								aint iNewline = Tail.f_Find("\n");
								if (iNewline >= 0)
									Tail = Tail.f_Extract(iNewline + 1);

								return Tail;
							}
							catch (CException const &)
							{
								return {};
							}
						}
					)
				;
			}

			if (LogTail.f_IsEmpty())
				co_return DMibErrorInstance("Failed to start the AppManager container system: {}"_f << LastError);

			co_return DMibErrorInstance
				(
					"Failed to start the AppManager container system: {}\n"
					"API server log tail:\n"
					"{}"_f
					<< LastError
					<< LogTail
				)
			;
		}

		// Containers cannot boot without a default kernel; install the recommended one
		// once per app root. The init filesystem image is pulled on demand
		if (!Prepared.m_bKernelInstalled)
		{
			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple
				(
					CProcessLaunchActor::CSimpleLaunch
						(
							fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("system", "kernel", "set", "--recommended", "--force"))
							, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
						)
				)
				.f_Wrap()
			;

			if (!Result)
				co_return DMibErrorInstance("Failed to install the default container kernel: {}"_f << Result.f_GetExceptionStr());

			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						g_Dispatch(BlockingActorCheckout) / [KernelMarkerFile]()
						{
							CFile::fs_WriteStringToFile(KernelMarkerFile, "installed");
						}
					)
				;
			}
		}

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
