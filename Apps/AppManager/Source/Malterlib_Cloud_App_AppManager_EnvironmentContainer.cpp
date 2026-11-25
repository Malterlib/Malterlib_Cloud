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

		// The agent would be process 1, which must reap the orphaned processes of the applications
		Arguments.f_Insert("--init");

		// The agent installs packages and creates the application accounts, whatever user the image selects
		Arguments.f_Insert("--user");
		Arguments.f_Insert("0:0");

		// Docker also resolves the host name on a user-defined network, which the administrator who shares the network with other services
		// chooses the host and environment names for
		if (!_Launch.m_Hostname.f_IsEmpty())
		{
			Arguments.f_Insert("--hostname");
			Arguments.f_Insert(_Launch.m_Hostname);
		}

		if (_Launch.m_bReadOnly)
		{
			Arguments.f_Insert("--read-only");

			// Programs in the environment expect a writable /tmp
			Arguments.f_Insert("--tmpfs");
			Arguments.f_Insert("/tmp");
		}

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

			// Whole limits without a decimal point, which the Apple container runtime needs; validation rejects fractions for it
			if (_Launch.m_CPULimit.f_Fraction() == 0.0)
				Arguments.f_Insert(CStr::fs_ToStr(_Launch.m_CPULimit.f_ToIntRoundTowardZero()));
			else
				Arguments.f_Insert(CStr::fs_ToStr(_Launch.m_CPULimit));
		}

		for (auto &Mount : _Launch.m_Mounts)
		{
			Arguments.f_Insert("--volume");
			Arguments.f_Insert("{}:{}"_f << _Launch.m_Mounts.fs_GetKey(Mount) << Mount);
		}

		// The Apple container runtime takes read-only binds only as mount options
		for (auto &Mount : _Launch.m_ReadOnlyMounts)
		{
			if (_Launch.m_bAppleContainer)
			{
				Arguments.f_Insert("--mount");
				Arguments.f_Insert("type=bind,source={},target={},readonly"_f << _Launch.m_ReadOnlyMounts.fs_GetKey(Mount) << Mount);
			}
			else
			{
				Arguments.f_Insert("--volume");
				Arguments.f_Insert("{}:{}:ro"_f << _Launch.m_ReadOnlyMounts.fs_GetKey(Mount) << Mount);
			}
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

		// The arguments after the image go to the entrypoint of the image, so the executable replaces the entrypoint
		Arguments.f_Insert("--entrypoint");
		Arguments.f_Insert(_Launch.m_Executable);

		Arguments.f_Insert(_Launch.m_Image);

		for (auto &Parameter : _Launch.m_Parameters)
			Arguments.f_Insert(Parameter);

		return Arguments;
	}

	// Docker reports other failures, like a missing context, as not found
	bool fg_AppManager_ContainerErrorReportsMissing(NStr::CStr const &_RuntimeExecutable, NStr::CStr const &_ErrorOutput)
	{
		if (_ErrorOutput.f_Find("No such container") >= 0)
			return true;

		return _RuntimeExecutable == "container" && _ErrorOutput.f_Find("not found") >= 0;
	}

	// Docker reports "No such volume" or "no such volume" depending on its version; the Apple container runtime reports "not found" from volume inspect
	bool fg_AppManager_ContainerVolumeErrorReportsMissing(NStr::CStr const &_RuntimeExecutable, NStr::CStr const &_ErrorOutput)
	{
		if (_ErrorOutput.f_FindNoCase("No such volume") >= 0)
			return true;

		return _RuntimeExecutable == "container" && _ErrorOutput.f_Find("not found") >= 0;
	}
}

namespace NMib::NCloud::NAppManager
{
	CStr CAppManagerActor::fp_GetContainerRuntimeExecutable(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		return fsp_GetContainerRuntimeExecutable(_pEnvironment->m_Settings);
	}

	CStr CAppManagerActor::fsp_GetContainerRuntimeExecutable(CEnvironmentSettings const &_Settings)
	{
		auto &Runtime = _Settings.m_ContainerRuntime;

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

		// Colima hosts the docker daemon, so container commands use the docker CLI
		if (Runtime == "Colima")
			return "docker";

		// Settings validation rejects other runtimes
		return Runtime;
	}

	CStr CAppManagerActor::fp_GetContainerName(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
		// Container runtimes are shared between AppManager installations, so the
		// name carries the installation transform used for user and group names
		return "mib-env-{}"_f << mp_pUniqueUserGroup->f_TransformUserGroup(_pEnvironment->m_Name);
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
		// The CLI and the AppManager-owned API server must use the same data root; the CLI reaches this server unless a login session runs its own
		_LaunchParams.m_Environment["CONTAINER_APP_ROOT"] = fp_GetAppleContainerAppRoot();

		// The AppManager can run as a daemon whose PATH misses the CLI install location
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;
		_LaunchParams.m_Environment["PATH"] = Path;
	}

	bool CAppManagerActor::fp_UseOwnColimaSystem(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
#ifdef DPlatformFamily_macOS
		return _pEnvironment->m_Settings.m_ContainerRuntime == "Colima";
#else
		return false;
#endif
	}

	bool CAppManagerActor::fsp_SettingsUseOwnColimaSystem(CEnvironmentSettings const &_Settings)
	{
#ifdef DPlatformFamily_macOS
		return _Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container && _Settings.m_ContainerRuntime == "Colima";
#else
		return false;
#endif
	}

	// The colima virtual machine shares the directories of every colima environment, so a change of the shares restarts the running colima
	// environments at the next start of one. Returns the running colima environments other than _ExcludeName
	TCVector<CStr> CAppManagerActor::fp_RunningColimaEnvironmentNames(CStr const &_ExcludeName)
	{
		TCVector<CStr> Names;
		for (auto &pEnvironment : mp_Environments)
		{
			if (pEnvironment->m_Name == _ExcludeName || !fp_UseOwnColimaSystem(pEnvironment))
				continue;

			if (pEnvironment->f_IsStarted() || pEnvironment->m_bStarting)
				Names.f_Insert(pEnvironment->m_Name);
		}

		return Names;
	}

	CStr CAppManagerActor::fp_GetColimaAppRoot()
	{
		return mp_State.m_RootDirectory / "Colima";
	}

	CStr CAppManagerActor::fp_GetColimaUser()
	{
#ifdef DPlatformFamily_macOS
		// limactl refuses to run as the root user, so a root AppManager runs the
		// colima virtual machine as a dedicated user. That user performs every
		// host-side file operation on the virtiofs mounts, no matter which user a
		// process inside the environment runs as
		if (CProcessLaunch::fs_GetElevation() == NProcess::EProcessElevation_IsRoot)
			return mp_pUniqueUserGroup->f_GetUser("MalterlibColima");
#endif
		return {};
	}

	CStr CAppManagerActor::fp_GetColimaGroup()
	{
#ifdef DPlatformFamily_macOS
		if (CProcessLaunch::fs_GetElevation() == NProcess::EProcessElevation_IsRoot)
			return mp_pUniqueUserGroup->f_GetGroup("MalterlibColima");
#endif
		return {};
	}

	void CAppManagerActor::fp_ApplyColimaLaunchEnvironment(CProcessLaunchParams &_LaunchParams)
	{
		CStr AppRoot = fp_GetColimaAppRoot();

		// The colima and docker clients must agree with the AppManager-owned virtual
		// machine on the data location; the explicit docker endpoint bypasses any
		// docker context configuration of the calling user
		_LaunchParams.m_Environment["COLIMA_HOME"] = AppRoot / "colima";
		_LaunchParams.m_Environment["DOCKER_CONFIG"] = AppRoot / "docker";
		_LaunchParams.m_Environment["DOCKER_HOST"] = "unix://" + (AppRoot / "colima" / "default" / "docker.sock");
		_LaunchParams.m_Environment["HOME"] = AppRoot / "home";

		// The AppManager can run as a daemon whose PATH misses the CLI install location
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;
		_LaunchParams.m_Environment["PATH"] = Path;
	}

	auto CAppManagerActor::fp_BuildContainerCommandParams(TCSharedPointer<CEnvironment> const &_pEnvironment, TCVector<CStr> &&_Arguments) -> CProcessLaunchParams
	{
		CStr Executable = fp_GetContainerRuntimeExecutable(_pEnvironment);

		bool bOwnAppleContainerSystem = Executable == "container" && fp_UseOwnAppleContainerSystem();

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable(Executable, fg_Move(_Arguments), mp_State.m_RootDirectory, {});
		LaunchParams.m_bAllowExecutableLocate = true;
		LaunchParams.m_bMergeEnvironment = true;

		if (bOwnAppleContainerSystem)
			fp_ApplyAppleContainerLaunchEnvironment(LaunchParams);
		else if (fp_UseOwnColimaSystem(_pEnvironment))
			fp_ApplyColimaLaunchEnvironment(LaunchParams);

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

						// The API server executable is beside the resolved CLI, and launchd validates its code signature
						// only by its real path
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
								"	<key>Label</key>\n"
								"	<string>com.apple.container.apiserver</string>\n"
								"	<key>ProgramArguments</key>\n"
								"	<array>\n"
								"		<string>{}</string>\n"
								"		<string>start</string>\n"
								"	</array>\n"
								"	<key>EnvironmentVariables</key>\n"
								"	<dict>\n"
								"		<key>CONTAINER_APP_ROOT</key>\n"
								"		<string>{}</string>\n"
								"		<key>CONTAINER_INSTALL_ROOT</key>\n"
								"		<string>{}</string>\n"
								"		<key>CONTAINER_LOG_ROOT</key>\n"
								"		<string>{}</string>\n"
								"	</dict>\n"
								"	<key>MachServices</key>\n"
								"	<dict>\n"
								"		<key>com.apple.container.apiserver</key>\n"
								"		<true/>\n"
								"	</dict>\n"
								"	<key>RunAtLoad</key>\n"
								"	<true/>\n"
								"	<key>LimitLoadToSessionType</key>\n"
								"	<array>\n"
								"		<string>Aqua</string>\n"
								"		<string>Background</string>\n"
								"		<string>System</string>\n"
								"	</array>\n"
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
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable("/bin/launchctl", fg_Move(_Arguments), mp_State.m_RootDirectory, {});
				LaunchParams.m_bMergeEnvironment = true;

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None);
				SimpleLaunch.m_LogName = "AppleContainer/Launchctl";

				return CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch);
			}
		;

		// The API service outlives the AppManager, so the label can already be loaded; the health check, not the bootstrap result, decides. The
		// system domain has one service of this label, so one AppManager on a host runs the Apple container runtime as root
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

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("list"))
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				)
			;
			SimpleLaunch.m_LogName = "AppleContainer/HealthCheck";

			auto Health = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

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

								// The byte limit can cut through the first log line
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
			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("system", "kernel", "set", "--recommended", "--force"))
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				)
			;
			SimpleLaunch.m_LogName = "AppleContainer/KernelInstall";

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

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

	TCFuture<void> CAppManagerActor::fp_EnsureColimaSystem(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (!fp_UseOwnColimaSystem(_pEnvironment))
			co_return {};

		// A removal only needs the machine running, so the shares stay until the next start drops those of the removed environment
		if (_pEnvironment->m_bRemoving && mp_bColimaSystemReady)
			co_return {};

		// The machine shares only what the colima environments mount, since a container that escapes into it reaches every share: their agent
		// directories read-only, their data directories, which hold the backing images of their application data, and the extra mounts the host
		// allowed them. The shares are fixed when the machine starts, so a changed set, like a new environment, restarts it. An environment whose
		// storage is unavailable, like closed encrypted storage, cannot start, and its directories are shared once it can
		TCMap<CStr, bool> Mounts; // Host path to whether the share is writable
		TCVector<CColimaEnvironmentDirectories> EnvironmentDirectories;
		for (auto &pOtherEnvironment : mp_Environments)
		{
			auto &Environment = *pOtherEnvironment;
			if (Environment.m_bDeleted || Environment.m_bRemoving)
				continue;

			if (Environment.m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container)
				continue;

			if (!fp_UseOwnColimaSystem(pOtherEnvironment))
				continue;

			CStr StorageError;
			CAppManagerInterface::EStatusSeverity StorageSeverity;
			if (!fp_EnvironmentStorageReady(Environment, StorageError, StorageSeverity))
				continue;

			auto &Directories = EnvironmentDirectories.f_Insert();
			Directories.m_StorageDirectory = fp_GetEnvironmentStorageDirectory(Environment);
			Directories.m_AgentDirectory = fp_GetEnvironmentAgentDirectory(Environment);
			Directories.m_DataDirectory = fp_GetEnvironmentDataDirectory(Environment);

			if (!Mounts.f_FindEqual(Directories.m_AgentDirectory))
				Mounts[Directories.m_AgentDirectory] = false;
			Mounts[Directories.m_DataDirectory] = true;

			for (auto &Mount : Environment.m_Settings.m_ContainerExtraMounts)
				Mounts[Environment.m_Settings.m_ContainerExtraMounts.fs_GetKey(Mount)] = true;
		}

		CStr ConfigFingerprint;
		for (auto &bWritable : Mounts)
			ConfigFingerprint += "{}{}\n"_f << Mounts.fs_GetKey(bWritable) << (bWritable ? ":w" : "");
		ConfigFingerprint += "CPUCount={}\n"_f << mp_ColimaCPUCount;
		ConfigFingerprint += "MemoryMB={}\n"_f << mp_ColimaMemoryMB;
		ConfigFingerprint += "DiskGB={}\n"_f << mp_ColimaDiskGB;

		if (mp_bColimaSystemReady && mp_ColimaConfigFingerprint == ConfigFingerprint)
			co_return {};

		if (mp_bColimaSystemStarting)
		{
			co_await mp_OnColimaSystemReady.f_Insert().f_Future();

			// The concurrent start can have applied a different configuration
			if (mp_ColimaConfigFingerprint == ConfigFingerprint)
				co_return {};

			co_return co_await fp_EnsureColimaSystem(_pEnvironment);
		}

		mp_bColimaSystemStarting = true;

		// The shares were computed before a removal that forgets them, so a removal while this start runs leaves the configuration unrecorded
		umint SharesGeneration = mp_ColimaSharesGeneration;

		bool bReady = false;
		auto Cleanup = g_OnScopeExit / [&, this]
			{
				mp_bColimaSystemStarting = false;
				mp_bColimaSystemReady = bReady;
				auto Waiters = fg_Move(mp_OnColimaSystemReady);
				for (auto &Promise : Waiters)
				{
					if (bReady)
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance("Failed to start the AppManager colima system"));
				}
			}
		;

		CStr AppRoot = fp_GetColimaAppRoot();
		CStr User = fp_GetColimaUser();
		CStr Group = fp_GetColimaGroup();
		CStr MarkerFile = AppRoot / ".MalterlibColimaConfig";

		struct CPrepareResult
		{
			CStr m_Error;
			CStr m_Info;
			CStr m_Executable;
			CStr m_PreviousConfig;
		};

		CPrepareResult Prepared;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Prepared = co_await
				(
					g_Dispatch(BlockingActorCheckout) / [AppRoot, User, Group, MarkerFile, EnvironmentDirectories]() -> CPrepareResult
					{
						CPrepareResult Result;

						for (auto &Candidate : {CStr("/usr/local/bin/colima"), CStr("/opt/homebrew/bin/colima")})
						{
							if (CFile::fs_FileExists(Candidate))
							{
								Result.m_Executable = Candidate;
								break;
							}
						}
						if (Result.m_Executable.f_IsEmpty())
							Result.m_Executable = NProcess::NPlatform::fg_FindExecutable("colima");
						if (Result.m_Executable.f_IsEmpty())
						{
							Result.m_Error = "The colima CLI was not found";
							return Result;
						}

						CFile::fs_CreateDirectory(AppRoot);
						CFile::fs_CreateDirectory(AppRoot / "colima");
						CFile::fs_CreateDirectory(AppRoot / "docker");
						CFile::fs_CreateDirectory(AppRoot / "home");

						if (CFile::fs_FileExists(MarkerFile))
							Result.m_PreviousConfig = CFile::fs_ReadStringFromFile(MarkerFile);

						if (!User.f_IsEmpty())
						{
							CStr GroupID;
							if (!NSys::fg_UserManagement_GroupExists(Group, GroupID))
							{
								NSys::fg_UserManagement_CreateGroup(Group, GroupID);
								Result.m_Info += fg_Format("Created group '{}' with resulting group ID: {}\n", Group, GroupID);
							}

							CStr UserID;
							if (!NSys::fg_UserManagement_UserExists(User, UserID))
							{
								NSys::fg_UserManagement_CreateUser(Group, User, "", User, AppRoot / "home", UserID, NSys::EUserManagementCreateUserFlag_None);
								Result.m_Info += fg_Format("Created user '{}' with resulting user ID: {}\n", User, UserID);
							}

							if (CFile::fs_GetOwner(AppRoot) != User)
								CFile::fs_SetOwnerAndGroupRecursive(AppRoot, User, Group);
						}

						// The machine can only share directories that exist
						for (auto &Directories : EnvironmentDirectories)
							fsp_PrepareColimaEnvironmentDirectories(Directories, User, Group);

						return Result;
					}
				)
			;
		}

		if (!Prepared.m_Error.f_IsEmpty())
			co_return DMibErrorInstance("Failed to start the AppManager colima system: {}"_f << Prepared.m_Error);

		if (!Prepared.m_Info.f_IsEmpty())
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "{}", CStr(Prepared.m_Info.f_Trim()));

		auto fColima = [this, &Prepared, &User, &Group](TCVector<CStr> &&_Arguments, auto _Flags)
			{
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable(Prepared.m_Executable, fg_Move(_Arguments), fp_GetColimaAppRoot(), {});
				LaunchParams.m_bMergeEnvironment = true;
				LaunchParams.m_RunAsUser = User;
				LaunchParams.m_RunAsGroup = Group;
				fp_ApplyColimaLaunchEnvironment(LaunchParams);

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, _Flags);
				SimpleLaunch.m_LogName = "Colima/System";

				return CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch);
			}
		;

		// Restart the collected environments also when the virtual machine restart fails
		TCVector<TCSharedPointer<CEnvironment>> RestartEnvironments;
		auto RestartCollected = g_OnScopeExit / [this, &RestartEnvironments]
			{
				for (auto &pEnvironment : RestartEnvironments)
				{
					if (pEnvironment->m_bDeleted || pEnvironment->m_bStoppedManually)
						continue;

					fp_EnsureEnvironmentStarted(pEnvironment) > [this, Name = pEnvironment->m_Name](TCAsyncResult<void> &&_Result)
						{
							if (!_Result)
								DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to start environment '{}' after the colima restart: {}", Name, _Result.f_GetExceptionStr());

							fp_UpdateApplicationDependencies();
						}
					;
				}
			}
		;

		bool bRunning = false;
		{
			auto Status = co_await fColima(fg_CreateVector<CStr>("status"), CProcessLaunchActor::ESimpleLaunchFlag_None).f_Wrap();
			bRunning = Status && Status->m_ExitCode == 0;
		}

		// A removal never restarts a running machine, which would restart every other colima environment, so it keeps the shares the machine
		// started with
		if (bRunning && _pEnvironment->m_bRemoving)
			ConfigFingerprint = Prepared.m_PreviousConfig;

		if (!bRunning || Prepared.m_PreviousConfig != ConfigFingerprint)
		{
			// The mount set and the virtual machine sizing can only change with a restart
			if (bRunning)
			{
				// Every running colima environment runs in the same virtual machine, also _pEnvironment when an image pull
				// ensures the system. A start that has not ensured the system yet waits for this one, so only a start that
				// has is waited for by the stop below; waiting for the others would deadlock
				for (auto &pEnvironment : mp_Environments)
				{
					if (!fp_UseOwnColimaSystem(pEnvironment))
						continue;

					if (pEnvironment->m_bStarting ? pEnvironment->m_bStartEnsuredContainerSystem : pEnvironment->f_IsStarted())
						RestartEnvironments.f_Insert(fg_TempCopy(pEnvironment));
				}

				for (auto &pEnvironment : RestartEnvironments)
				{
					co_await fp_WaitForEnvironmentIdle(pEnvironment);
					co_await fp_StopEnvironmentInternal(pEnvironment);
				}

				// A machine that still runs would take the start below as done with its old shares
				auto StopResult = co_await fColima(fg_CreateVector<CStr>("stop"), CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode).f_Wrap();
				if (!StopResult)
				{
					auto Status = co_await fColima(fg_CreateVector<CStr>("status"), CProcessLaunchActor::ESimpleLaunchFlag_None).f_Wrap();
					if (!Status || Status->m_ExitCode == 0)
						co_return DMibErrorInstance("Failed to stop the AppManager colima system to change its configuration: {}"_f << StopResult.f_GetExceptionStr());

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "colima stop failed, but the machine stopped: {}", StopResult.f_GetExceptionStr());
				}
			}

			// colima keeps the sizing of its last start in its profile, so the default sizing is passed explicitly: the 2 CPUs and 2 GiB of memory
			// that colima defaults to. A disk can't shrink, so its default only applies to a new machine
			TCVector<CStr> StartArguments = fg_CreateVector<CStr>("start", "--vm-type", "vz", "--mount-type", "virtiofs");
			StartArguments.f_Insert("--cpu");
			StartArguments.f_Insert("{}"_f << (mp_ColimaCPUCount ? mp_ColimaCPUCount : 2));
			StartArguments.f_Insert("--memory");
			StartArguments.f_Insert(CStr::fs_ToStr(fp64(mp_ColimaMemoryMB ? mp_ColimaMemoryMB : 2048) / 1024.0));
			if (mp_ColimaDiskGB)
			{
				StartArguments.f_Insert("--disk");
				StartArguments.f_Insert("{}"_f << mp_ColimaDiskGB);
			}
			for (auto &bWritable : Mounts)
			{
				StartArguments.f_Insert("--mount");
				StartArguments.f_Insert("{}{}"_f << Mounts.fs_GetKey(bWritable) << (bWritable ? ":w" : ""));
			}

			auto StartResult = co_await fColima(fg_Move(StartArguments), CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode).f_Wrap();
			if (!StartResult)
				co_return DMibErrorInstance("Failed to start the AppManager colima system: {}"_f << StartResult.f_GetExceptionStr());

			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						(
							g_Dispatch(BlockingActorCheckout) / [MarkerFile, ConfigFingerprint]()
							{
								CFile::fs_WriteStringToFile(MarkerFile, ConfigFingerprint);
							}
						)
						% "Failed to save the colima configuration state"
					)
				;
			}

			// The removal deleted the marker before the write above can have created it again
			if (SharesGeneration != mp_ColimaSharesGeneration)
				co_await fp_ForgetColimaShares().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to reset the colima configuration state");
		}

		CStr LastError;
		bool bHealthy = false;
		for (aint Attempt = 0; !bHealthy && Attempt < 6; ++Attempt)
		{
			if (Attempt != 0)
				co_await fg_Timeout(2.0);

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("ps"))
					, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
				)
			;
			SimpleLaunch.m_LogName = "Colima/HealthCheck";

			auto Health = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

			if (Health)
				bHealthy = true;
			else
				LastError = Health.f_GetExceptionStr();
		}

		if (!bHealthy)
			co_return DMibErrorInstance("The docker daemon in the colima virtual machine is not responding: {}"_f << LastError);

		if (SharesGeneration == mp_ColimaSharesGeneration)
			mp_ColimaConfigFingerprint = ConfigFingerprint;

		bReady = true;

		co_return {};
	}

	// The machine keeps sharing the directories it started with also once they are deleted, so a directory created again at a shared path needs a
	// restart of the machine, which the next start makes once the recorded configuration is gone
	TCFuture<void> CAppManagerActor::fp_ForgetColimaShares()
	{
		++mp_ColimaSharesGeneration;
		mp_ColimaConfigFingerprint.f_Clear();

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [MarkerFile = fp_GetColimaAppRoot() / ".MalterlibColimaConfig"]()
					{
						if (CFile::fs_FileExists(MarkerFile))
							CFile::fs_DeleteFile(MarkerFile);
					}
				)
				% "Failed to reset the colima configuration state"
			)
		;

		co_return {};
	}

	// The colima user performs the host side of the virtiofs mounts of the colima virtual machine, so it traverses the environment storage to the
	// read-only agent directory and owns the data directory, where the virtual machine creates the backing image of the application data. The
	// ownership changes only the directory, since the virtual machine can place links in it. Blocking
	void CAppManagerActor::fsp_PrepareColimaEnvironmentDirectories(CColimaEnvironmentDirectories const &_Directories, CStr const &_User, CStr const &_Group)
	{
		if (!CFile::fs_FileExists(_Directories.m_AgentDirectory, EFileAttrib_Directory))
		{
			CFile::fs_CreateDirectory(_Directories.m_AgentDirectory);
			CFile::fs_SetAttributes
				(
					_Directories.m_AgentDirectory
					, EFileAttrib_UnixAttributesValid
					| EFileAttrib_UserRead
					| EFileAttrib_UserWrite
					| EFileAttrib_UserExecute
					| EFileAttrib_GroupRead
					| EFileAttrib_GroupExecute
					| EFileAttrib_EveryoneRead
					| EFileAttrib_EveryoneExecute
				)
			;
		}

		// Only its owner reaches the data directory, which keeps other host users away from the backing image of the application data. The
		// storage directory above it is traversable for the colima user
		if (!CFile::fs_FileExists(_Directories.m_DataDirectory, EFileAttrib_Directory))
			CFile::fs_CreateDirectory(_Directories.m_DataDirectory);
		CFile::fs_SetAttributes(_Directories.m_DataDirectory, EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute);

		if (_User.f_IsEmpty())
			return;

		CFile::fs_SetAttributes
			(
				_Directories.m_StorageDirectory
				, (CFile::fs_GetAttributes(_Directories.m_StorageDirectory) & ~EFileAttrib_Directory) | EFileAttrib_UnixAttributesValid | EFileAttrib_EveryoneExecute
			)
		;

		CFile::fs_SetOwner(_Directories.m_DataDirectory, _User);
		if (!_Group.f_IsEmpty())
			CFile::fs_SetGroup(_Directories.m_DataDirectory, _Group);
	}

	TCFuture<void> CAppManagerActor::fp_PrepareColimaDataDirectory(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CColimaEnvironmentDirectories Directories;
		Directories.m_StorageDirectory = fp_GetEnvironmentStorageDirectory(*_pEnvironment);
		Directories.m_AgentDirectory = fp_GetEnvironmentAgentDirectory(*_pEnvironment);
		Directories.m_DataDirectory = fp_GetEnvironmentDataDirectory(*_pEnvironment);

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [Directories, User = fp_GetColimaUser(), Group = fp_GetColimaGroup()]()
					{
						fsp_PrepareColimaEnvironmentDirectories(Directories, User, Group);
					}
				)
				% "Failed to prepare the data directory for the colima user"
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_EnsureContainerSystem(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (fp_UseOwnColimaSystem(_pEnvironment))
			co_return co_await fp_EnsureColimaSystem(_pEnvironment);

		co_return co_await fp_EnsureAppleContainerSystem(_pEnvironment);
	}

	// The container gets the host-written agent directory read-only, its data storage read-write and nothing else of the host but the agent socket
	// and the extra mounts the host allowed. Sets o_Error when the launch can't be built
	auto CAppManagerActor::fp_BuildEnvironmentContainerLaunch(TCSharedPointer<CEnvironment> const &_pEnvironment, CStr const &_AgentExecutableName, CStr &o_Error)
		-> CAppManagerContainerLaunch
	{
		auto &Settings = _pEnvironment->m_Settings;

		bool bAppleContainer = fp_GetContainerRuntimeExecutable(_pEnvironment) == "container";

		CStr AgentRoot = mcp_pEnvironmentAgentRootContainer;
		CStr StateRoot = CStr(mcp_pEnvironmentDataRootContainer) / "Agent";

		CAppManagerContainerLaunch Launch;
		Launch.m_ContainerName = fp_GetContainerName(_pEnvironment);

		// Docker's default capabilities keep the container from changing its host name, so it is set at creation. The name tells hosts apart, not
		// the AppManagers of one host
		Launch.m_Hostname = fp_GetEnvironmentHostName(*_pEnvironment, o_Error);
		if (!o_Error.f_IsEmpty())
			return {};

		Launch.m_Image = Settings.m_ContainerImage;
		Launch.m_MemoryLimitMB = Settings.m_MemoryLimitMB;
		Launch.m_CPULimit = Settings.m_CPULimit;
		Launch.m_bReadOnly = Settings.m_bContainerReadOnly;
		Launch.m_ExtraArguments = Settings.m_ContainerExtraArguments;
		Launch.m_WorkingDirectory = AgentRoot;

		// A restarted container runs the agent files the Current link points to then
		Launch.m_Executable = AgentRoot / "Current" / _AgentExecutableName;
		Launch.m_Parameters = {"--daemon-run-standalone", "--log-to-stderr"};

		// An empty setting selects the default network of the runtime, which other environments share. The agent reaches the AppManager through
		// the agent socket or the environment listen
		Launch.m_Network = Settings.m_ContainerNetwork;

		Launch.m_ReadOnlyMounts[fp_GetEnvironmentAgentDirectory(*_pEnvironment)] = AgentRoot;
		Launch.m_bAppleContainer = bAppleContainer;

		// The application storage survives container recreation. Colima shares only the backing image of it through virtiofs, which its virtual
		// machine loop mounts
		if (fp_EnvironmentUsesHostDataDirectory(*_pEnvironment))
			Launch.m_Mounts[fp_GetEnvironmentDataDirectory(*_pEnvironment) / "Root"] = mcp_pEnvironmentDataRootContainer;
		else if (fp_UseOwnColimaSystem(_pEnvironment))
			Launch.m_Mounts[fp_GetEnvironmentDataVolumeMountPath(*_pEnvironment)] = mcp_pEnvironmentDataRootContainer;
		else
			Launch.m_Mounts[fp_GetEnvironmentDataVolumeName(*_pEnvironment)] = mcp_pEnvironmentDataRootContainer;

		// Mounting the directory lets the container see the socket a restarted AppManager recreates; connecting needs no write access
		if (fp_EnvironmentUsesAgentSocket(_pEnvironment))
		{
			CStr SocketDirectory = fp_GetEnvironmentAgentSocketDirectory(*_pEnvironment).m_SocketDirectory;
			Launch.m_ReadOnlyMounts[SocketDirectory] = SocketDirectory;
		}

		for (auto &Mount : Settings.m_ContainerExtraMounts)
			Launch.m_Mounts[Settings.m_ContainerExtraMounts.fs_GetKey(Mount)] = Mount;

		// The Apple container runtime has no --add-host option
		if (Launch.m_Network != "host" && !bAppleContainer)
		{
			CStr Host = mp_State.m_LocalAddress.f_GetHost();
			if (!Host.f_IsEmpty() && !Host.f_StartsWith("UNIX("))
				Launch.m_AddHosts[Host] = "host-gateway";
		}

		// The environment carries only launch and connection parameters, since other processes in the container can inherit it; the agent identity is in its deployment settings
		Launch.m_PassEnvironment =
			{
				"MalterlibDistributedAppInterfaceServerAddress"
				, "MalterlibDistributedAppInterfaceServerRequestTicket"
				, "MalterlibDistributedAppInterfaceServerLaunchID"
				, "MalterlibDistributedAppInterfaceServerOptions"
				, "MalterlibProtectedEnvironment"
				, "HOME={}/.home"_f << StateRoot
				, "TMPDIR={}/.tmp"_f << StateRoot
			}
		;

		return Launch;
	}

	// Linux hosts keep the application data of a container in a directory of the environment storage, on the file system of the host applications and
	// inside the directory of a parent application, which can be encrypted. On macOS the runtimes cross a virtual machine, whose own file systems hold
	// the data
	bool CAppManagerActor::fp_EnvironmentUsesHostDataDirectory(CEnvironment const &_Environment)
	{
#ifdef DPlatformFamily_macOS
		return false;
#else
		return _Environment.m_Settings.m_Type == CAppManagerInterface::EEnvironmentType_Container;
#endif
	}

	CStr CAppManagerActor::fp_GetEnvironmentDataVolumeName(CEnvironment const &_Environment)
	{
		return "mib-env-{}-data"_f << mp_pUniqueUserGroup->f_TransformUserGroup(_Environment.m_Name);
	}

	CStr CAppManagerActor::fp_GetEnvironmentDataVolumeImagePath(CEnvironment const &_Environment)
	{
		// Keep the backing image in the host environment storage so it survives a colima virtual machine rebuild
		return fp_GetEnvironmentDataDirectory(_Environment) / "DataDisk.img";
	}

	CStr CAppManagerActor::fp_GetEnvironmentDataVolumeMountPath(CEnvironment const &_Environment)
	{
		return "/mnt/malterlib/{}"_f << fp_GetEnvironmentDataVolumeName(_Environment);
	}

	auto CAppManagerActor::fp_RunColimaVMScript(CStr _Script, CStr _LogName) -> TCFuture<CProcessLaunchActor::CSimpleLaunchResult>
	{
		co_return co_await fp_RunColima(fg_CreateVector<CStr>("ssh", "--", "sudo", "/bin/sh", "-c", fg_Move(_Script)), fg_Move(_LogName));
	}

	auto CAppManagerActor::fp_RunColima(TCVector<CStr> _Arguments, CStr _LogName) -> TCFuture<CProcessLaunchActor::CSimpleLaunchResult>
	{
		CStr User = fp_GetColimaUser();
		CStr Group = fp_GetColimaGroup();

		CStr Executable;
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			Executable = co_await
				(
					g_Dispatch(BlockingActorCheckout) / []() -> CStr
					{
						for (auto &Candidate : {CStr("/usr/local/bin/colima"), CStr("/opt/homebrew/bin/colima")})
						{
							if (CFile::fs_FileExists(Candidate))
								return fg_TempCopy(Candidate);
						}

						return NProcess::NPlatform::fg_FindExecutable("colima");
					}
				)
			;
		}

		if (Executable.f_IsEmpty())
			co_return DMibErrorInstance("The colima CLI was not found");

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable(Executable, fg_Move(_Arguments), fp_GetColimaAppRoot(), {});
		LaunchParams.m_bMergeEnvironment = true;
		LaunchParams.m_RunAsUser = User;
		LaunchParams.m_RunAsGroup = Group;
		fp_ApplyColimaLaunchEnvironment(LaunchParams);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None);
		SimpleLaunch.m_LogName = fg_Move(_LogName);

		co_return co_await CProcessLaunchActor::fs_LaunchSimple(fg_Move(SimpleLaunch));
	}

	TCFuture<void> CAppManagerActor::fp_EnsureEnvironmentDataVolume(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// Only the host can reach the data directory, which keeps host users away from the files the container creates, like set-user-ID
		// executables. The container gets the directory inside it
		if (fp_EnvironmentUsesHostDataDirectory(*_pEnvironment))
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [DataDirectory = fp_GetEnvironmentDataDirectory(*_pEnvironment)]()
						{
							if (!CFile::fs_FileExists(DataDirectory, EFileAttrib_Directory))
								CFile::fs_CreateDirectory(DataDirectory);
							CFile::fs_SetAttributes(DataDirectory, EFileAttrib_UnixAttributesValid | EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UserExecute);

							// The application accounts of the container traverse the root of the data storage
							if (!CFile::fs_FileExists(DataDirectory / "Root", EFileAttrib_Directory))
							{
								CFile::fs_CreateDirectory(DataDirectory / "Root");
								CFile::fs_SetAttributes
									(
										DataDirectory / "Root"
										, EFileAttrib_UnixAttributesValid
										| EFileAttrib_UserRead
										| EFileAttrib_UserWrite
										| EFileAttrib_UserExecute
										| EFileAttrib_GroupRead
										| EFileAttrib_GroupExecute
										| EFileAttrib_EveryoneRead
										| EFileAttrib_EveryoneExecute
									)
								;
							}
						}
					)
					% "Failed to create the environment data directory"
				)
			;

			co_return {};
		}

		CStr VolumeName = fp_GetEnvironmentDataVolumeName(*_pEnvironment);

		bool bColima = fp_UseOwnColimaSystem(_pEnvironment);

		// A named volume lives in the runtime's own storage, outside the parent application's possibly encrypted directory
		if (!bColima && !_pEnvironment->m_Settings.m_ParentApplication.f_IsEmpty())
			co_return DMibErrorInstance("Environments with a parent application need the Colima container runtime on this host");

		CStr ImagePath;
		if (bColima)
			ImagePath = fp_GetEnvironmentDataVolumeImagePath(*_pEnvironment);

		if (!bColima)
		{
			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("volume", "inspect", VolumeName))
					, CProcessLaunchActor::ESimpleLaunchFlag_None
				)
			;
			SimpleLaunch.m_LogName = "Environment/{}/DataVolumeInspect"_f << _pEnvironment->m_Name;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
			if (Result && Result->m_ExitCode == 0)
				co_return {};
		}

		if (bColima)
		{
			co_await fp_PrepareColimaDataDirectory(_pEnvironment);

			// The application data crosses the virtiofs mount only as this image, so the guest kernel handles the ownership and permissions inside it
			uint64 SizeGB = _pEnvironment->m_Settings.m_VMDataDiskGB ? _pEnvironment->m_Settings.m_VMDataDiskGB : 64;

			CStr MountPath = fp_GetEnvironmentDataVolumeMountPath(*_pEnvironment);

			CStr Script = fg_Format
				(
					"set -e\n"
					"Image={}\n"
					"Mount={}\n"
					"if [ ! -f \"$Image\" ]; then\n"
					"	if ! command -v mkfs.xfs >/dev/null 2>&1; then\n"
					"		export DEBIAN_FRONTEND=noninteractive\n"
					"		apt-get update -q\n"
					"		apt-get install -q -y xfsprogs\n"
					"	fi\n"
					"	rm -f \"$Image.tmp\"\n"
					"	truncate -s {}G \"$Image.tmp\"\n"
					"	mkfs.xfs -q \"$Image.tmp\"\n"
					"	mv \"$Image.tmp\" \"$Image\"\n"
					"fi\n"
					"mkdir -p \"$Mount\"\n"
					"if ! mountpoint -q \"$Mount\"; then\n"
					"	mount -t xfs -o loop \"$Image\" \"$Mount\"\n"
					"fi\n"
					, NStr::fg_StrEscapeBashSingleQuotes(ImagePath)
					, NStr::fg_StrEscapeBashSingleQuotes(MountPath)
					, SizeGB
				)
			;

			auto Result = co_await fp_RunColimaVMScript(fg_Move(Script), "Environment/{}/DataVolumeImage"_f << _pEnvironment->m_Name).f_Wrap();
			if (!Result)
				co_return DMibErrorInstance("Failed to prepare the data volume image '{}': {}"_f << ImagePath << Result.f_GetExceptionStr());
			if (Result->m_ExitCode != 0)
			{
				co_return DMibErrorInstance
					(
						"Preparing the data volume image '{}' failed with status {}: {}"_f << ImagePath << Result->m_ExitCode << CStr(Result->f_GetCombinedOut().f_Trim())
					)
				;
			}

			co_return {};
		}

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("volume", "create", VolumeName))
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/DataVolumeCreate"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

		if (!Result)
			co_return DMibErrorInstance("Failed to create the environment data volume '{}': {}"_f << VolumeName << Result.f_GetExceptionStr());

		co_return {};
	}

	// Whether the runtime holds the application data: a named volume, or the backing image that the Colima virtual machine mounts until a removal
	// through Colima. Fails when the runtime can't tell whether the volume exists
	TCFuture<bool> CAppManagerActor::fp_EnvironmentDataVolumeExists(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (_pEnvironment->m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container || fp_EnvironmentUsesHostDataDirectory(*_pEnvironment))
			co_return false;

		if (fp_UseOwnColimaSystem(_pEnvironment))
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_return co_await
				(
					g_Dispatch(BlockingActorCheckout) / [ImagePath = fp_GetEnvironmentDataVolumeImagePath(*_pEnvironment)]()
					{
						return CFile::fs_FileExists(ImagePath) || CFile::fs_FileExists(ImagePath + ".tmp");
					}
				)
			;
		}

		co_await (fp_EnsureContainerSystem(_pEnvironment) % "Failed to start the container system");

		CStr VolumeName = fp_GetEnvironmentDataVolumeName(*_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("volume", "inspect", VolumeName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/DataVolumeInspect"_f << _pEnvironment->m_Name;

		auto Result = co_await (CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch) % CStr("Failed to inspect the data volume '{}'"_f << VolumeName));
		if (Result.m_ExitCode == 0)
			co_return true;

		CStr ErrorOutput = CStr(Result.f_GetCombinedOut().f_Trim());
		if (fg_AppManager_ContainerVolumeErrorReportsMissing(fp_GetContainerRuntimeExecutable(_pEnvironment), ErrorOutput))
			co_return false;

		co_return DMibErrorInstance("Failed to inspect the data volume '{}': {}"_f << VolumeName << ErrorOutput);
	}

	// Removes the application data that the container runtime holds or that only it can delete; the caller deletes the rest of the environment storage after it
	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentDataVolume(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// The files the container writes belong to its accounts, like root, which the host AppManager need not be, so a short-lived container of the
		// image deletes them. It runs in a mount namespace of its own, where links in the data resolve inside the container, and rm does not follow
		// them anyway. The caller has removed the container of the environment, and deletes the emptied data directory
		if (fp_EnvironmentUsesHostDataDirectory(*_pEnvironment))
		{
			CStr DataDirectory = fp_GetEnvironmentDataDirectory(*_pEnvironment);

			bool bHasRoot;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				bHasRoot = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [DataDirectory]()
						{
							return CFile::fs_FileExists(DataDirectory / "Root", EFileAttrib_Directory | EFileAttrib_File | EFileAttrib_Link);
						}
					)
				;
			}

			if (!bHasRoot)
				co_return {};

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams
					(
						_pEnvironment
						, fg_CreateVector<CStr>
						(
							"run"
							, "--rm"
							, "--user"
							, "0:0"
							, "--network"
							, "none"
							, "--volume"
							, "{}:/Data"_f << DataDirectory
							, "--entrypoint"
							, "rm"
							, _pEnvironment->m_Settings.m_ContainerImage
							, "-rf"
							, "/Data/Root"
						)
					)
					, CProcessLaunchActor::ESimpleLaunchFlag_None
				)
			;
			SimpleLaunch.m_LogName = "Environment/{}/DataDelete"_f << _pEnvironment->m_Name;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
			if (!Result)
				co_return DMibErrorInstance("Failed to delete the data of environment '{}', so it is kept: {}"_f << _pEnvironment->m_Name << Result.f_GetExceptionStr());

			if (Result->m_ExitCode != 0)
			{
				co_return DMibErrorInstance
					(
						"Deleting the data of environment '{}' failed with status {}, so it is kept: {}"_f
						<< _pEnvironment->m_Name
						<< Result->m_ExitCode
						<< CStr(Result->f_GetCombinedOut().f_Trim())
					)
				;
			}

			co_return {};
		}

		// Colima environments keep their data in the backing image below instead of a named volume
		if (!fp_UseOwnColimaSystem(_pEnvironment))
		{
			CStr VolumeName = fp_GetEnvironmentDataVolumeName(*_pEnvironment);

			CProcessLaunchActor::CSimpleLaunch SimpleLaunch
				(
					fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("volume", "rm", VolumeName))
					, CProcessLaunchActor::ESimpleLaunchFlag_None
				)
			;
			SimpleLaunch.m_LogName = "Environment/{}/DataVolumeRemove"_f << _pEnvironment->m_Name;

			auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

			// The caller asked for the data to be deleted, so only a volume that is already gone counts as removed
			if (!Result)
				co_return DMibErrorInstance("Failed to remove the data volume '{}', so the application data is kept: {}"_f << VolumeName << Result.f_GetExceptionStr());

			if (Result->m_ExitCode != 0)
			{
				CStr RuntimeExecutable = fp_GetContainerRuntimeExecutable(_pEnvironment);
				CStr ErrorOutput = CStr(Result->f_GetCombinedOut().f_Trim());
				bool bMissing = fg_AppManager_ContainerVolumeErrorReportsMissing(RuntimeExecutable, ErrorOutput);

				// The Apple container runtime reports a missing volume to volume rm like any other failure, and only volume inspect tells it apart
				if (!bMissing && RuntimeExecutable == "container")
				{
					CProcessLaunchActor::CSimpleLaunch InspectLaunch
						(
							fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("volume", "inspect", VolumeName))
							, CProcessLaunchActor::ESimpleLaunchFlag_None
						)
					;
					InspectLaunch.m_LogName = "Environment/{}/DataVolumeInspect"_f << _pEnvironment->m_Name;

					auto InspectResult = co_await CProcessLaunchActor::fs_LaunchSimple(InspectLaunch).f_Wrap();
					if (!InspectResult)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to inspect the data volume '{}': {}", VolumeName, InspectResult.f_GetExceptionStr());
					else if (InspectResult->m_ExitCode != 0)
					{
						CStr InspectOutput = CStr(InspectResult->f_GetCombinedOut().f_Trim());
						bMissing = fg_AppManager_ContainerVolumeErrorReportsMissing(RuntimeExecutable, InspectOutput);
						if (bMissing)
							ErrorOutput = InspectOutput;
					}
				}

				if (!bMissing)
					co_return DMibErrorInstance("Failed to remove the data volume '{}', so the application data is kept: {}"_f << VolumeName << ErrorOutput);

				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "The data volume '{}' was already removed: {}", VolumeName, ErrorOutput);
			}
		}

		// The backing image lives on the host, so it is removed there directly;
		// this also works when the colima virtual machine is not running, in
		// which case nothing is mounted either and the script can fail freely.
		// A mount that stays active keeps the image: a removed image would stay
		// mounted, and an environment added again with the same name would
		// reuse that mount
		if (fp_UseOwnColimaSystem(_pEnvironment))
		{
			CStr ImagePath = fp_GetEnvironmentDataVolumeImagePath(*_pEnvironment);
			CStr MountPath = fp_GetEnvironmentDataVolumeMountPath(*_pEnvironment);

			auto UnmountResult = co_await fp_RunColimaVMScript
				(
					fg_Format
					(
						"Mount={}\n"
						"if mountpoint -q \"$Mount\" && ! umount \"$Mount\"; then\n"
						"	echo MalterlibDataVolumeStillMounted\n"
						"	exit 1\n"
						"fi\n"
						"rmdir \"$Mount\" 2>/dev/null || true\n"
						, NStr::fg_StrEscapeBashSingleQuotes(MountPath)
					)
					, "Environment/{}/DataVolumeUnmount"_f << _pEnvironment->m_Name
				)
				.f_Wrap()
			;

			if (UnmountResult && UnmountResult->m_ExitCode != 0 && UnmountResult->f_GetStdOut().f_Find("MalterlibDataVolumeStillMounted") >= 0)
			{
				co_return DMibErrorInstance
					(
						"The data volume image of environment '{}' is still mounted in the colima virtual machine, so it is kept: {}"_f
						<< _pEnvironment->m_Name
						<< CStr(UnmountResult->f_GetCombinedOut().f_Trim())
					)
				;
			}

			// The script can also fail without reaching the virtual machine, which only leaves nothing mounted when it is not running. colima
			// status fails only for a virtual machine that isn't running
			if (!UnmountResult || UnmountResult->m_ExitCode != 0)
			{
				CStr UnmountError = UnmountResult ? CStr(UnmountResult->f_GetCombinedOut().f_Trim()) : UnmountResult.f_GetExceptionStr();

				auto StatusResult = co_await fp_RunColima(fg_CreateVector<CStr>("status"), "Environment/{}/DataVolumeUnmountStatus"_f << _pEnvironment->m_Name).f_Wrap();
				if (StatusResult && StatusResult->m_ExitCode == 0)
				{
					co_return DMibErrorInstance
						(
							"Failed to unmount the data volume image of environment '{}' in the running colima virtual machine, so it is kept: {}"_f << _pEnvironment->m_Name << UnmountError
						)
					;
				}

				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Failed to unmount the data volume image of environment '{}', whose colima virtual machine is not running: {}"
						, _pEnvironment->m_Name
						, UnmountError
					)
				;
			}

			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [ImagePath]()
						{
							if (CFile::fs_FileExists(ImagePath))
								CFile::fs_DeleteFile(ImagePath);
							if (CFile::fs_FileExists(ImagePath + ".tmp"))
								CFile::fs_DeleteFile(ImagePath + ".tmp");
						}
					)
					% "Failed to remove the data volume image, so the application data is kept"
				)
			;
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (_pEnvironment->m_ContainerFingerprint.f_IsEmpty())
			co_return {};

		co_return co_await fp_RemoveEnvironmentContainerByName(_pEnvironment);
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentContainerByName(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("rm", "--force", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerRemove"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
		if (Result && Result->m_ExitCode == 0)
			co_return {};

		// A container that remained would keep the identity of its launch while the caller records a new one
		if (Result && fg_AppManager_ContainerErrorReportsMissing(fp_GetContainerRuntimeExecutable(_pEnvironment), Result->f_GetErrorOut()))
			co_return {};

		co_return DMibErrorInstance("Failed to remove container '{}': {}"_f << ContainerName << (Result ? CStr(Result->f_GetErrorOut().f_Trim()) : Result.f_GetExceptionStr()));
	}

	TCFuture<void> CAppManagerActor::fp_StopEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (_pEnvironment->m_ContainerFingerprint.f_IsEmpty())
			co_return {};

		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("stop", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerStop"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();
		if (Result && Result->m_ExitCode == 0)
			co_return {};

		if (co_await fp_OwnContainerSystemStopped(_pEnvironment))
			co_return {};

		// Stopping a container that does not exist fails as well. A runtime that cannot be reached fails both commands,
		// so otherwise only a stop that reports the container missing counts as stopped
		if
		(
			!co_await fp_EnvironmentContainerExists(_pEnvironment)
			&& Result
			&& fg_AppManager_ContainerErrorReportsMissing(fp_GetContainerRuntimeExecutable(_pEnvironment), Result->f_GetErrorOut())
		)
		{
			co_return {};
		}

		co_return DMibErrorInstance("Failed to stop container '{}': {}"_f << ContainerName << (Result ? CStr(Result->f_GetErrorOut().f_Trim()) : Result.f_GetExceptionStr()));
	}

	// Kills the container without the grace period of a stop, which its processes could use to hold up the stop
	TCFuture<void> CAppManagerActor::fp_KillEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		if (_pEnvironment->m_ContainerFingerprint.f_IsEmpty())
			co_return {};

		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("kill", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerKill"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch);
		if (Result.m_ExitCode != 0)
			co_return DMibErrorInstance("Failed to kill container '{}': {}"_f << ContainerName << Result.f_GetErrorOut().f_Trim());

		co_return {};
	}

	// True when the container system that the AppManager runs for _pEnvironment is known not to run, so it runs none of its containers. It doesn't
	// run after a host restart until an environment starts, and the stops of environments that aren't running don't start it
	TCFuture<bool> CAppManagerActor::fp_OwnContainerSystemStopped(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		// colima status fails only for a virtual machine that isn't running
		if (fp_UseOwnColimaSystem(_pEnvironment))
		{
			auto Status = co_await fp_RunColima(fg_CreateVector<CStr>("status"), "Environment/{}/ContainerSystemStatus"_f << _pEnvironment->m_Name).f_Wrap();
			co_return Status && Status->m_ExitCode != 0;
		}

		if (fp_GetContainerRuntimeExecutable(_pEnvironment) != "container" || !fp_UseOwnAppleContainerSystem())
			co_return false;

		// The API server runs the containers, and only a loaded service can run it
		constexpr uint32 c_LaunchctlServiceNotFound = 113;

		CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
			(
				"/bin/launchctl"
				, fg_CreateVector<CStr>("print", "system/com.apple.container.apiserver")
				, mp_State.m_RootDirectory
				, {}
			)
		;
		LaunchParams.m_bMergeEnvironment = true;

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None);
		SimpleLaunch.m_LogName = "Environment/{}/ContainerSystemStatus"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

		co_return Result && Result->m_ExitCode == c_LaunchctlServiceNotFound;
	}

	TCFuture<bool> CAppManagerActor::fp_EnvironmentContainerExists(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		// A plain docker inspect also finds the images, networks and volumes of the name, like a network that an administrator named after the
		// environment; the inspect of the Apple runtime looks only at containers
		TCVector<CStr> Arguments;
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) == "container")
			Arguments = fg_CreateVector<CStr>("inspect", ContainerName);
		else
			Arguments = fg_CreateVector<CStr>("container", "inspect", ContainerName);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch(fp_BuildContainerCommandParams(_pEnvironment, fg_Move(Arguments)), CProcessLaunchActor::ESimpleLaunchFlag_None);
		SimpleLaunch.m_LogName = "Environment/{}/ContainerInspect"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

		co_return Result && Result->m_ExitCode == 0;
	}

	TCFuture<void> CAppManagerActor::fp_PullEnvironmentContainerImage(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		TCVector<CStr> Arguments;
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) == "container")
			Arguments = fg_CreateVector<CStr>("image", "pull", _pEnvironment->m_Settings.m_ContainerImage);
		else
			Arguments = fg_CreateVector<CStr>("pull", _pEnvironment->m_Settings.m_ContainerImage);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_Move(Arguments))
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerPull"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

		if (!Result)
			co_return DMibErrorInstance("Failed to pull image '{}': {}"_f << _pEnvironment->m_Settings.m_ContainerImage << Result.f_GetExceptionStr());

		co_return {};
	}
}
