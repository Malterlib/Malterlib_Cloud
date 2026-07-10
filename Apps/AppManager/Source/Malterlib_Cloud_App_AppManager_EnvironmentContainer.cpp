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

		if (!_Launch.m_Hostname.f_IsEmpty())
		{
			Arguments.f_Insert("--hostname");
			Arguments.f_Insert(_Launch.m_Hostname);
		}

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

		// The colima runtime is docker with an AppManager-owned colima virtual machine
		// hosting the daemon
		if (Runtime == "Colima")
			return "docker";

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

	bool CAppManagerActor::fp_UseOwnColimaSystem(TCSharedPointer<CEnvironment> const &_pEnvironment)
	{
#ifdef DPlatformFamily_macOS
		return _pEnvironment->m_Settings.m_ContainerRuntime == "Colima";
#else
		return false;
#endif
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

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, CProcessLaunchActor::ESimpleLaunchFlag_None);
				SimpleLaunch.m_LogName = "AppleContainer/Launchctl";

				return CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch);
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

		// The virtual machine can only share host paths configured when it starts, so
		// it is started with every path the colima environments bind mount into their
		// containers and restarted whenever that set changes. The whole root directory
		// is shared so that the app set can change without a restart; the containers
		// only see the bind mounts they were created with, not the machine's shares
		TCSet<CStr> Mounts;
		Mounts.f_Insert(fg_TempCopy(mp_State.m_RootDirectory));
		for (auto &pOtherEnvironment : mp_Environments)
		{
			auto &Environment = *pOtherEnvironment;
			if (Environment.m_bDeleted)
				continue;

			if (Environment.m_Settings.m_Type != CAppManagerInterface::EEnvironmentType_Container)
				continue;

			if (!fp_UseOwnColimaSystem(pOtherEnvironment))
				continue;

			// Environment storage inside a parent application directory can be a
			// separate mount not visible through the root directory share
			if (!Environment.m_Settings.m_ParentApplication.f_IsEmpty())
				Mounts.f_Insert(fp_GetEnvironmentStorageDirectory(Environment));

			// Application directories outside the root directory are not visible
			// through the root directory share either
			for (auto &pApplication : mp_Applications)
			{
				if (pApplication->m_bDeleted)
					continue;

				if (pApplication->m_Settings.m_LaunchEnvironment != Environment.m_Name)
					continue;

				CStr Directory = pApplication->f_GetDirectory();
				if (Directory == mp_State.m_RootDirectory || Directory.f_StartsWith(mp_State.m_RootDirectory + "/"))
					continue;

				Mounts.f_Insert(fg_Move(Directory));
			}

			for (auto &Mount : Environment.m_Settings.m_ContainerExtraMounts)
				Mounts.f_Insert(fg_TempCopy(Environment.m_Settings.m_ContainerExtraMounts.fs_GetKey(Mount)));
		}

		CStr ConfigFingerprint;
		for (auto &Mount : Mounts)
			ConfigFingerprint += Mount + "\n";
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
					g_Dispatch(BlockingActorCheckout) / [AppRoot, User, Group, MarkerFile]() -> CPrepareResult
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
								NSys::fg_UserManagement_CreateUser
									(
										Group
										, User
										, ""
										, User
										, AppRoot / "home"
										, UserID
										, NSys::EUserManagementCreateUserFlag_None
									)
								;
								Result.m_Info += fg_Format("Created user '{}' with resulting user ID: {}\n", User, UserID);
							}

							if (CFile::fs_GetOwner(AppRoot) != User)
								CFile::fs_SetOwnerAndGroupRecursive(AppRoot, User, Group);
						}

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
				CProcessLaunchParams LaunchParams = CProcessLaunchParams::fs_LaunchExecutable
					(
						Prepared.m_Executable
						, fg_Move(_Arguments)
						, fp_GetColimaAppRoot()
						, {}
					)
				;
				LaunchParams.m_bMergeEnvironment = true;
				LaunchParams.m_RunAsUser = User;
				LaunchParams.m_RunAsGroup = Group;
				fp_ApplyColimaLaunchEnvironment(LaunchParams);

				CProcessLaunchActor::CSimpleLaunch SimpleLaunch(LaunchParams, _Flags);
				SimpleLaunch.m_LogName = "Colima/System";

				return CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch);
			}
		;

		bool bRunning = false;
		{
			auto Status = co_await fColima(fg_CreateVector<CStr>("status"), CProcessLaunchActor::ESimpleLaunchFlag_None).f_Wrap();
			bRunning = Status && Status->m_ExitCode == 0;
		}

		if (!bRunning || Prepared.m_PreviousConfig != ConfigFingerprint)
		{
			// The mount set and the virtual machine sizing can only change with a restart
			if (bRunning)
			{
				co_await fColima(fg_CreateVector<CStr>("stop"), CProcessLaunchActor::ESimpleLaunchFlag_None).f_Wrap()
					> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop the colima virtual machine")
				;
			}

			TCVector<CStr> StartArguments = fg_CreateVector<CStr>("start", "--vm-type", "vz", "--mount-type", "virtiofs");
			if (mp_ColimaCPUCount)
			{
				StartArguments.f_Insert("--cpu");
				StartArguments.f_Insert("{}"_f << mp_ColimaCPUCount);
			}
			if (mp_ColimaMemoryMB)
			{
				StartArguments.f_Insert("--memory");
				StartArguments.f_Insert(CStr::fs_ToStr(fp64(mp_ColimaMemoryMB) / 1024.0));
			}
			if (mp_ColimaDiskGB)
			{
				StartArguments.f_Insert("--disk");
				StartArguments.f_Insert("{}"_f << mp_ColimaDiskGB);
			}
			for (auto &Mount : Mounts)
			{
				StartArguments.f_Insert("--mount");
				StartArguments.f_Insert("{}:w"_f << Mount);
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

		mp_ColimaConfigFingerprint = ConfigFingerprint;
		bReady = true;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_EnsureColimaOwnership(CStr _Directory)
	{
		CStr User = fp_GetColimaUser();
		if (User.f_IsEmpty())
			co_return {};

		CStr Group = fp_GetColimaGroup();

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [_Directory, User, Group]()
					{
						// Host-side file operations on the colima virtiofs mounts run as
						// the colima user, so everything the environment writes to must
						// be owned by it
						CFile::fs_SetOwnerAndGroupRecursive(_Directory, User, Group);
					}
				)
				% "Failed to change owner to the colima user"
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

		// Docker does not let the container change its host name at runtime, so it
		// is set at creation; the Apple container runtime has no hostname option and
		// the agent sets it instead, which its virtual machine allows
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) != "container")
			Launch.m_Hostname = fp_GetEnvironmentHostName(*_pEnvironment);

		Launch.m_Image = Settings.m_ContainerImage;
		Launch.m_MemoryLimitMB = Settings.m_MemoryLimitMB;
		Launch.m_CPULimit = Settings.m_CPULimit;
		Launch.m_bReadOnly = Settings.m_bContainerReadOnly;
		Launch.m_ExtraArguments = Settings.m_ContainerExtraArguments;
		Launch.m_WorkingDirectory = _AgentRootDirectory;
		Launch.m_Executable = _AgentExecutable;
		Launch.m_Parameters = {"--daemon-run-standalone", "--log-to-stderr"};

		if (!Settings.m_ContainerNetwork.f_IsEmpty())
			Launch.m_Network = Settings.m_ContainerNetwork;
#ifdef DPlatformFamily_Linux
		else
			Launch.m_Network = "host";
#endif

		// The container gets the minimum mount surface: the environment storage, the
		// directories of the applications launched in the environment and the
		// configured extra mounts. Every path is mounted at its host path so that
		// application directories stay valid inside the container, and the rest of
		// the host root directory stays invisible to the environment
		Launch.m_Mounts[_AgentRootDirectory] = _AgentRootDirectory;

		for (auto &pApplication : mp_Applications)
		{
			if (pApplication->m_bDeleted)
				continue;

			if (pApplication->m_Settings.m_LaunchEnvironment != _pEnvironment->m_Name)
				continue;

			CStr Directory = pApplication->f_GetDirectory();
			Launch.m_Mounts[Directory] = Directory;
		}

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
		// The agent identity, host id and host name come from the deployment
		// settings next to the agent executable, not from the environment: the
		// container environment is inherited by everything running inside it
		Launch.m_PassEnvironment =
			{
				"MalterlibDistributedAppInterfaceServerAddress"
				, "MalterlibDistributedAppInterfaceServerRequestTicket"
				, "MalterlibDistributedAppInterfaceServerLaunchID"
				, "MalterlibDistributedAppInterfaceServerOptions"
				, "MalterlibProtectedEnvironment"
				, "HOME={}/.home"_f << _AgentRootDirectory
				, "TMPDIR={}/.tmp"_f << _AgentRootDirectory
			}
		;

		// Unix domain sockets cannot live on the virtiofs shares the Apple container
		// runtime and colima use for mounts, so the agent places its local command
		// line socket on the container filesystem instead
		if (fp_GetContainerRuntimeExecutable(_pEnvironment) == "container" || fp_UseOwnColimaSystem(_pEnvironment))
			Launch.m_PassEnvironment.f_Insert("MalterlibDistributedAppLocalSocketPrefix=/tmp");

		return Launch;
	}

	TCFuture<void> CAppManagerActor::fp_RemoveEnvironmentContainer(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("rm", "--force", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerRemove"_f << _pEnvironment->m_Name;

		auto Result = co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap();

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

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("stop", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
		SimpleLaunch.m_LogName = "Environment/{}/ContainerStop"_f << _pEnvironment->m_Name;

		co_await CProcessLaunchActor::fs_LaunchSimple(SimpleLaunch).f_Wrap()
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop environment container")
		;

		co_return {};
	}

	TCFuture<bool> CAppManagerActor::fp_EnvironmentContainerExists(TCSharedPointer<CEnvironment> _pEnvironment)
	{
		CStr ContainerName = fp_GetContainerName(_pEnvironment);

		CProcessLaunchActor::CSimpleLaunch SimpleLaunch
			(
				fp_BuildContainerCommandParams(_pEnvironment, fg_CreateVector<CStr>("inspect", ContainerName))
				, CProcessLaunchActor::ESimpleLaunchFlag_None
			)
		;
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
