// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Cryptography/RandomID>

#include <Mib/Concurrency/LogError>
#include <Mib/Cloud/FileTransfer>

#ifdef DPlatformFamily_Linux
#	include <Mib/Core/PlatformSpecific/PosixErrNo>
#	include <unistd.h>
#endif

#ifdef DPlatformFamily_macOS
#	include <sys/param.h>
#	include <sys/mount.h>
#endif

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud
{
	bool fg_AppManager_HostsFileHasHostName(NStr::CStr const &_Hosts, NStr::CStr const &_HostName)
	{
		for (auto &Line : _Hosts.f_SplitLine<true>())
		{
			ch8 const *pParse = Line.f_GetStr();
			bool bAddress = true;
			while (true)
			{
				NStr::fg_ParseWhiteSpace(pParse);
				if (!*pParse || *pParse == '#')
					break;

				ch8 const *pStart = pParse;
				NStr::fg_ParseNonWhiteSpaceAndSeparators(pParse, "#");

				if (!bAddress && NStr::CStr(pStart, pParse - pStart).f_CmpNoCase(_HostName) == 0)
					return true;

				bAddress = false;
			}
		}

		return false;
	}
}

namespace NMib::NCloud::NAppManager
{
	CAppManagerEnvironmentInterface::CAppManagerEnvironmentInterface()
	{
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_GetAgentInfo);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_LaunchApplication);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_StopApplication);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_RunScript);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_StageApplicationFiles);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_ApplyApplicationFiles);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_DiscardApplicationStage);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_RemoveApplicationStorage);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_ConfigureAgent);
		DPublishActorFunction(CAppManagerEnvironmentInterface::f_ShutdownEnvironment);
	}

	CAppManagerEnvironmentInterface::~CAppManagerEnvironmentInterface()
	{
	}

	CAppManagerEnvironmentHostInterface::CAppManagerEnvironmentHostInterface()
	{
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_RegisterEnvironmentAgent);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_ReportApplicationState);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_RequestApplicationConnectionTicket);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_ReportEnvironmentUpdateState);
		DPublishActorFunction(CAppManagerEnvironmentHostInterface::f_RequestEnvironmentRestart);
	}

	CAppManagerEnvironmentHostInterface::~CAppManagerEnvironmentHostInterface()
	{
	}

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CAgentInfo::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Platform;
		_Stream % m_PlatformFamily;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CAgentInfo);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CEnvironmentLaunch::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Name;
		_Stream % m_Directory;
		_Stream % m_Executable;
		_Stream % m_Parameters;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_bRunAsUserHasShell;
		_Stream % m_RunAsUserPassword;
		_Stream % m_bDistributedApp;
		_Stream % m_InterfaceAddress;
		_Stream % m_LaunchID;
		_Stream % m_OSDependencies;
		_Stream % m_LowPortExecutables;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CEnvironmentLaunch);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CEnvironmentScript::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Description;
		_Stream % m_Script;
		_Stream % m_Application;
		_Stream % m_Directory;
		_Stream % m_Parameter;
		_Stream % m_Environment;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_RunAsUserPassword;
		_Stream % m_bRunAsUserHasShell;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CEnvironmentScript);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CStageFileContents::f_Stream(tf_CStream &_Stream)
	{
		_Stream % fg_Move(m_DataGenerator);
		_Stream % fg_Move(m_Subscription);
		_Stream % m_StartPosition;
	}

	// Always streams the symlink target: the version manager file type only streams it in its own protocol versions
	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CStageFile::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_FilePath;
		_Stream % m_FileAttributes;
		_Stream % m_WriteTime;
		_Stream % m_FileSize;
		_Stream % m_SymlinkContents;
		_Stream % fg_Move(m_fGetDataGenerator);
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CStageFile);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CApplicationStage::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Name;
		_Stream % m_QueueSize;

		if constexpr (tf_CStream::mc_Direction == NStream::EStreamDirection_Consume)
			m_FilesGenerator = fg_Construct();

		_Stream % fg_Move(*m_FilesGenerator);
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_bRunAsUserHasShell;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CApplicationStage);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CApplicationStageResult::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_StageID;
		_Stream % m_Directory;
		_Stream % m_StageDirectory;
		_Stream % fg_Move(m_fFinish);
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CApplicationStageResult);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CApplicationApply::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Name;
		_Stream % m_StageID;
		_Stream % m_Files;
		_Stream % m_OldFiles;
		_Stream % m_RunAsUser;
		_Stream % m_RunAsGroup;
		_Stream % m_bRunAsUserHasShell;
		_Stream % m_bFailOnExisting;
		_Stream % m_VersionExtraInfo;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CApplicationApply);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CApplicationStateChange::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_Application;
		_Stream % m_Status;
		_Stream % m_StatusSeverity;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CApplicationStateChange);

	template <typename tf_CStream>
	void CAppManagerEnvironmentInterface::CAgentConfig::f_Stream(tf_CStream &_Stream)
	{
		_Stream % m_AutoUpdateConfig;
		_Stream % m_OSDependencies;
		_Stream % m_AgentOSDependencies;
	}
	DMibDistributedStreamImplement(CAppManagerEnvironmentInterface::CAgentConfig);

	auto CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_GetAgentInfo() -> TCFuture<CAgentInfo>
	{
		CAgentInfo Info;
		Info.m_Platform = DMalterlibCloudPlatform;
		Info.m_PlatformFamily = DMibStringize(DPlatformFamily);

		co_return fg_Move(Info);
	}

#ifdef DPlatformFamily_Linux
	namespace
	{
		// Blocking. The capabilities are best effort, like for host applications: a minimal image can lack setcap and a file system can reject
		// capabilities, and an application can run without them. The agent of a Local environment runs as root on the host, so low port
		// executables that are links or below one are skipped, as for host applications, see fg_AppManager_LowPortExecutablePath
		void fg_SetLowPortExecutableCapabilities(CStr const &_Directory, TCVector<CStr> const &_LowPortExecutables)
		{
			for (auto &Executable : _LowPortExecutables)
			{
				// The lookup of the path reads the file system, which can fail like setcap
				try
				{
					CStr ExecutablePath = fg_AppManager_LowPortExecutablePath(_Directory, Executable);
					if (ExecutablePath.f_IsEmpty())
						continue;

					CProcessLaunch::fs_LaunchTool("setcap", {"cap_net_bind_service=ep", ExecutablePath});
				}
				catch (CException const &_Exception)
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Warning
							, "Failed to set up low port executable support running 'setcap cap_net_bind_service=ep {}': {}"
							, _Directory / Executable
							, _Exception
						)
					;
				}
			}
		}
	}
#endif

#ifdef DPlatformFamily_macOS
	namespace
	{
		// Blocking. Accepts any volume already mounted at _DataRoot; otherwise mounts the volume of that name or, on first use, formats the guest's data disk as APFS
		void fg_PrepareEnvironmentDataVolume(CStr const &_DataRoot)
		{
			CStr VolumeName = CFile::fs_GetFile(_DataRoot);

			auto fIsMounted = [&]() -> bool
				{
					struct statfs FileSystemInfo = {};
					return statfs(_DataRoot.f_GetStr(), &FileSystemInfo) == 0 && _DataRoot == FileSystemInfo.f_mntonname;
				}
			;

			auto fRunDiskUtil = [](TCVector<CStr> &&_Arguments, CStr &o_Output) -> bool
				{
					CStr StdOut;
					CStr StdErr;
					uint32 ExitCode = 0;
					if (!CProcessLaunch::fs_LaunchBlock("/usr/sbin/diskutil", fg_Move(_Arguments), StdOut, StdErr, ExitCode))
						DMibError(fg_Format("Failed to launch diskutil: {}", StdErr));

					o_Output = fg_ConcatOutput(StdOut, StdErr);
					return ExitCode == 0;
				}
			;

			auto fEnableOwnership = [&]
				{
					CStr Output;
					if (!fRunDiskUtil(fg_CreateVector<CStr>("enableOwnership", _DataRoot), Output))
						DMibError(fg_Format("Failed to enable ownership on the data volume '{}': {}", _DataRoot, Output));
				}
			;

			if (fIsMounted())
				return;

			// The volume can already exist unmounted, for example after a manual
			// unmount; ownership must be honored so the per application users work
			{
				CStr Output;
				if (fRunDiskUtil(fg_CreateVector<CStr>("mount", VolumeName), Output) && fIsMounted())
				{
					fEnableOwnership();
					return;
				}
			}

			// Find the attached data disk: the only whole physical disk without any
			// content. The main guest disk always carries a partition scheme
			CStr Candidate;
			{
				CStr StdOut;
				CStr StdErr;
				uint32 ExitCode = 0;
				if
				(
					!CProcessLaunch::fs_LaunchBlock("/bin/sh", fg_CreateVector<CStr>("-c", "diskutil list -plist physical | plutil -convert json -o - -"), StdOut, StdErr, ExitCode)
					|| ExitCode != 0
				)
				{
					DMibError(fg_Format("Failed to list the guest disks: {}", fg_ConcatOutput(StdOut, StdErr)));
				}

				CJsonOrdered Json = CJsonOrdered::fs_FromString(StdOut);

				umint nCandidates = 0;
				if (auto *pDisks = Json.f_GetMember("AllDisksAndPartitions"))
				{
					for (auto &Disk : pDisks->f_Array())
					{
						CStr Content;
						if (auto *pContent = Disk.f_GetMember("Content"))
							Content = pContent->f_AsString();

						bool bHasPartitions = false;
						if (auto *pPartitions = Disk.f_GetMember("Partitions"))
							bHasPartitions = !pPartitions->f_Array().f_IsEmpty();
						if (Disk.f_GetMember("APFSVolumes"))
							bHasPartitions = true;

						if (!Content.f_IsEmpty() || bHasPartitions)
							continue;

						++nCandidates;
						if (auto *pIdentifier = Disk.f_GetMember("DeviceIdentifier"))
							Candidate = pIdentifier->f_AsString();
					}
				}

				if (Candidate.f_IsEmpty())
					DMibError("Found no unformatted data disk to create the data volume on");

				if (nCandidates > 1)
					DMibError("Found more than one unformatted disk; cannot decide which one is the data disk");
			}

			{
				CStr Output;
				if (!fRunDiskUtil(fg_CreateVector<CStr>("eraseDisk", "APFS", VolumeName, Candidate), Output))
					DMibError(fg_Format("Failed to format the data disk '{}': {}", Candidate, Output));

				fEnableOwnership();
			}

			if (!fIsMounted())
				DMibError(fg_Format("The data volume did not mount at '{}'", _DataRoot));
		}
	}
#endif

	TCFuture<void> CAppManagerActor::fp_EnsureEnvironmentDataRoot()
	{
		if (mp_EnvironmentDataRoot.f_IsEmpty())
			co_return DMibErrorInstance("The environment has no managed application storage");

		if (mp_bEnvironmentDataRootReady)
			co_return {};

		if (mp_bEnvironmentDataRootPreparing)
			co_return co_await mp_OnEnvironmentDataRootReady.f_Insert().f_Future();

		mp_bEnvironmentDataRootPreparing = true;

		bool bReady = false;
		auto Cleanup = g_OnScopeExit / [&, this]
			{
				mp_bEnvironmentDataRootPreparing = false;
				mp_bEnvironmentDataRootReady = bReady;
				auto Waiters = fg_Move(mp_OnEnvironmentDataRootReady);
				for (auto &Promise : Waiters)
				{
					if (bReady)
						Promise.f_SetResult();
					else
						Promise.f_SetException(DMibErrorInstance("Failed to prepare the environment application storage"));
				}
			}
		;

		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [DataRoot = mp_EnvironmentDataRoot, bContainer = mp_bEnvironmentContainer]()
						{
							// A container runtime mounts the data root volume; a macOS guest prepares its data disk volume itself
#ifdef DPlatformFamily_macOS
							if (!bContainer)
							{
								fg_PrepareEnvironmentDataVolume(DataRoot);
								return;
							}
#else
							(void)bContainer;
#endif

							if (!CFile::fs_FileExists(DataRoot, EFileAttrib_Directory))
								CFile::fs_CreateDirectory(DataRoot);
						}
					)
					% "Failed to prepare the environment application storage"
				)
			;
		}

		bReady = true;

		co_return {};
	}

	CStr CAppManagerActor::fp_GetAgentApplicationDirectory(CStr const &_Name)
	{
		return mp_EnvironmentDataRoot / "App" / _Name;
	}

	// Returns the launch id of a matching application that is already launched or launching instead of _Launch's
	TCFuture<CStr> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_LaunchApplication(CEnvironmentLaunch _Launch)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (_Launch.m_Name.f_IsEmpty() || _Launch.m_Executable.f_IsEmpty())
			co_return DMibErrorInstance("Invalid launch of application in environment: name and executable are required");

		bool bManagedStorage = _Launch.m_Directory.f_IsEmpty();

		if (bManagedStorage)
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Launch.m_Name))
				co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Launch.m_Name);

			co_await pThis->fp_EnsureEnvironmentDataRoot();

			_Launch.m_Directory = pThis->fp_GetAgentApplicationDirectory(_Launch.m_Name);

			bool bHasDirectory;
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				bHasDirectory = co_await
					(
						g_Dispatch(BlockingActorCheckout) / [Directory = _Launch.m_Directory]()
						{
							return CFile::fs_FileExists(Directory, EFileAttrib_Directory);
						}
					)
				;
			}

			if (!bHasDirectory)
				co_return DMibErrorInstance("Application '{}' has no files in the environment storage; update the application to install them"_f << _Launch.m_Name);
		}

		// A process that is stopping is not adopted: a successful stop removes the application
		while (true)
		{
			auto *pStopping = pThis->mp_Applications.f_FindEqual(_Launch.m_Name);
			if (!pStopping || !(*pStopping)->m_bPendingStop)
				break;

			auto pStoppingApplication = *pStopping;
			auto StopResult = co_await pStoppingApplication->m_OnStops.f_Insert().f_Future().f_Wrap();
			if (!StopResult)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Stop of '{}' before its launch failed: {}", _Launch.m_Name, StopResult.f_GetExceptionStr());
		}

		auto *pFindApplication = pThis->mp_Applications.f_FindEqual(_Launch.m_Name);
		if (pFindApplication)
		{
			auto pExisting = *pFindApplication;
			if (pExisting->f_IsLaunched() || pExisting->m_bLaunching)
			{
				auto &ExistingSettings = pExisting->m_Settings;

				bool bSameLaunch
					= pExisting->m_DirectoryOverride == _Launch.m_Directory
					&& ExistingSettings.m_Executable == _Launch.m_Executable
					&& ExistingSettings.m_ExecutableParameters == _Launch.m_Parameters
					&& ExistingSettings.m_RunAsUser == _Launch.m_RunAsUser
					&& ExistingSettings.m_RunAsGroup == _Launch.m_RunAsGroup
					&& ExistingSettings.m_bDistributedApp == _Launch.m_bDistributedApp
				;

				// A surviving process with a matching launch is adopted with the launch ID it runs with, so the host can match its registration
				if (bSameLaunch)
					co_return fg_TempCopy(pExisting->m_EnvironmentHostLaunchID);

				co_return DMibErrorInstance("Application '{}' is already launched in the environment with different settings"_f << _Launch.m_Name);
			}

			pExisting->f_Delete();
			pThis->mp_Applications.f_Remove(_Launch.m_Name);
		}

		auto pApplication = pThis->mp_Applications[_Launch.m_Name] = fg_Construct(_Launch.m_Name, pThis);

		auto &Application = *pApplication;
		Application.m_bEphemeral = true;

		// Dependency updates could launch the entry while it is prepared, which would fail the launch below
		Application.m_bPreventLaunch_User = true;
		Application.m_DirectoryOverride = _Launch.m_Directory;
		Application.m_EnvironmentHostAddress = _Launch.m_InterfaceAddress;
		Application.m_EnvironmentHostLaunchID = _Launch.m_LaunchID;

		auto &Settings = Application.m_Settings;
		Settings.m_Executable = _Launch.m_Executable;
		Settings.m_ExecutableParameters = _Launch.m_Parameters;
		Settings.m_RunAsUser = _Launch.m_RunAsUser;
		Settings.m_RunAsGroup = _Launch.m_RunAsGroup;
		Settings.m_bRunAsUserHasShell = _Launch.m_bRunAsUserHasShell;
#ifdef DPlatformFamily_Windows
		Settings.m_RunAsUserPassword = _Launch.m_RunAsUserPassword;
#endif
		Settings.m_bDistributedApp = _Launch.m_bDistributedApp;
		Settings.m_OSDependencies = fg_Move(_Launch.m_OSDependencies);
		Settings.m_bBackupEnabled = false;
		Settings.m_bAutoUpdate = false;

		// A stop can delete the entry while it is prepared, and a new launch of the name can then prepare it with other settings. The launch below
		// is outside, since a failed launch stops and removes the entry
		{
			auto OnResume = co_await fg_OnResume
				(
					[pApplication]() -> CExceptionPointer
					{
						if (pApplication->m_bDeleted)
							return DMibErrorInstance("Application was deleted while launching");

						return {};
					}
				)
			;

			// The accounts are created, and the capabilities of the low port executables set, with tools the dependencies can provide
			co_await (pThis->fp_EnsureOSDependencies(pApplication) % "Failed to install the OS dependencies of the application");

			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						(
							g_Dispatch(BlockingActorCheckout) / [Settings, Directory = _Launch.m_Directory, pUniqueUserGroup = pThis->mp_pUniqueUserGroup]()
							{
								fsp_CreateApplicationUserGroup
									(
										Settings
										, [](CStr const &_Info)
										{
											DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment agent launch: {}", _Info);
										}
										, Directory / ".home"
										, pUniqueUserGroup
									)
								;
							}
						)
						% "Failed to prepare the user and group for application in environment"
					)
				;
			}

			// Only the agent reaches the environment storage to change file owners for a new run as identity
			if (bManagedStorage)
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						(
							g_Dispatch(BlockingActorCheckout) /
							[
								Directory = _Launch.m_Directory
								, User = pThis->mp_pUniqueUserGroup->f_GetUser(Settings.m_RunAsUser)
								, Group = pThis->mp_pUniqueUserGroup->f_GetGroup(Settings.m_RunAsGroup)
							]
							()
							{
								fsp_ReassignApplicationFileOwnership(Directory, User, Group);
							}
						)
						% "Failed to change the owners of the application files in the environment storage"
					)
				;
			}

#ifdef DPlatformFamily_Linux
			// The OS dependencies the launch installed can provide setcap, and changing the owners clears the capabilities
			if (!_Launch.m_LowPortExecutables.f_IsEmpty())
			{
				auto BlockingActorCheckout = fg_BlockingActor();

				co_await
					(
						(
							g_Dispatch(BlockingActorCheckout) / [Directory = _Launch.m_Directory, LowPortExecutables = _Launch.m_LowPortExecutables]()
							{
								fg_SetLowPortExecutableCapabilities(Directory, LowPortExecutables);
							}
						)
						% "Failed to set the capabilities of the low port executables"
					)
				;
			}
#endif
		}

		pApplication->m_bPreventLaunch_User = false;
		CAppLaunchResult Result = co_await pThis->fp_LaunchApp(pApplication, false);

		if (Result.m_StartupError)
		{
			// The host retries the launch periodically; remove the failed ephemeral
			// application so the retry does not hit the already-launched check
			if (!pApplication->m_bDeleted)
			{
				co_await pApplication->f_Stop(EStopFlag_PreventLaunchUser).f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to stop application after failed environment launch");

				pApplication->f_AbortPendingLaunches();
				pApplication->f_Delete();

				auto *pFindCurrent = pThis->mp_Applications.f_FindEqual(_Launch.m_Name);
				if (pFindCurrent && *pFindCurrent == pApplication)
					pThis->mp_Applications.f_Remove(_Launch.m_Name);
			}

			co_return DMibErrorInstance(fg_Move(Result.m_StartupError));
		}

		co_return fg_Move(_Launch.m_LaunchID);
	}

	// Stopping an application that does not exist, such as one cleaned up after a failed launch, succeeds with exit status 0
	TCFuture<uint32> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_StopApplication(CStr _Name)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		auto *pFindApplication = pThis->mp_Applications.f_FindEqual(_Name);
		if (!pFindApplication)
			co_return 0;

		auto pApplication = *pFindApplication;

		uint32 ExitStatus = co_await pApplication->f_Stop(EStopFlag_PreventLaunchUser);

		// A launch waiting for the stop can have replaced and deleted the application already
		if (!pApplication->m_bDeleted)
		{
			pApplication->f_AbortPendingLaunches();
			pApplication->f_Delete();
		}

		if (auto *pCurrent = pThis->mp_Applications.f_FindEqual(_Name); pCurrent && *pCurrent == pApplication)
			pThis->mp_Applications.f_Remove(_Name);

		co_return ExitStatus;
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_RunScript(CEnvironmentScript _Script)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (_Script.m_Directory.f_IsEmpty() && !_Script.m_Application.f_IsEmpty())
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Script.m_Application))
				co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Script.m_Application);

			co_await pThis->fp_EnsureEnvironmentDataRoot();

			_Script.m_Directory = pThis->fp_GetAgentApplicationDirectory(_Script.m_Application);

			if (!_Script.m_Environment.f_FindEqual("HOME"))
				_Script.m_Environment["HOME"] = _Script.m_Directory + "/.home";
			if (!_Script.m_Environment.f_FindEqual("TMPDIR"))
				_Script.m_Environment["TMPDIR"] = _Script.m_Directory + "/.tmp";
		}

		_Script.m_RunAsUser = pThis->mp_pUniqueUserGroup->f_GetUser(_Script.m_RunAsUser);
		_Script.m_RunAsGroup = pThis->mp_pUniqueUserGroup->f_GetGroup(_Script.m_RunAsGroup);

		CStrSecure RunAsUserPassword = fg_Move(_Script.m_RunAsUserPassword);
		co_return co_await pThis->fp_RunBashScript(fg_Move(_Script), fg_Move(RunAsUserPassword));
	}

	namespace
	{
		struct COptionalFuture
		{
			COptionalFuture(TCFuture<void> &&_Future);
			COptionalFuture(COptionalFuture &&_Other) = default;
			~COptionalFuture();

			TCFuture<void> m_Future;
		};

		COptionalFuture::COptionalFuture(TCFuture<void> &&_Future)
			: m_Future(fg_Move(_Future))
		{
		}

		COptionalFuture::~COptionalFuture()
		{
			if (m_Future.f_IsValid())
				m_Future.f_DiscardResult();
		}
	}

	// Returns once the file transfer has started; m_fFinish of the result waits for the transfer and for the staged files to get their owner
	auto CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_StageApplicationFiles(CApplicationStage _Stage) -> TCFuture<CApplicationStageResult>
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (!CVersionManager::fs_IsValidApplicationName(_Stage.m_Name))
			co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Stage.m_Name);

		if (!_Stage.m_FilesGenerator || !_Stage.m_FilesGenerator->f_IsValid())
			co_return DMibErrorInstance("The application stage carries no files generator");

		co_await pThis->fp_EnsureEnvironmentDataRoot();

		// Preparing the stage deletes the stages of the application, which an apply the host stopped waiting for can still move files from
		co_await pThis->fp_WaitForApplicationFilesApply(_Stage.m_Name);

		CApplicationStageResult Result;
		Result.m_StageID = fg_FastRandomID();
		Result.m_Directory = pThis->fp_GetAgentApplicationDirectory(_Stage.m_Name);
		Result.m_StageDirectory = Result.m_Directory / "TempVersion" / Result.m_StageID;

		// The host can disappear after a receive finishes, leaving a stage behind
		{
			auto BlockingActorCheckout = fg_BlockingActor();

			co_await
				(
					(
						g_Dispatch(BlockingActorCheckout) / [Directory = Result.m_Directory, StageDirectory = Result.m_StageDirectory]()
						{
							CStr StageRoot = Directory / "TempVersion";
							if (CFile::fs_FileExists(StageRoot))
								CFile::fs_DeleteDirectoryRecursive(StageRoot);

							CFile::fs_CreateDirectory(StageDirectory);
						}
					)
					% "Failed to prepare the application stage directory"
				)
			;
		}

		auto &StageReceive = pThis->mp_ApplicationStageReceives[Result.m_StageID];
		StageReceive.m_Name = _Stage.m_Name;
		StageReceive.m_StageDirectory = Result.m_StageDirectory;
		StageReceive.m_Receive = fg_ConstructActor<CFileTransferReceive>
			(
				Result.m_StageDirectory
				, EFileAttrib_UnixAttributesValid
				| EFileAttrib_UserRead
				| EFileAttrib_UserWrite
				| EFileAttrib_UserExecute
				| EFileAttrib_GroupRead
				| EFileAttrib_GroupExecute
				| EFileAttrib_EveryoneRead
				| EFileAttrib_EveryoneExecute
				, EFileAttrib_UserRead | EFileAttrib_UserWrite | EFileAttrib_UnixAttributesValid
			)
		;

		auto ReceiveFilesFuture = StageReceive.m_Receive
			(
				&CFileTransferReceive::f_ReceiveFiles
				, CFileTransferSendDownloadFile::fs_TranslateGenerator<CFileTransferSendDownloadFile>(fg_Move(*_Stage.m_FilesGenerator))
				, _Stage.m_QueueSize
				, CFileTransferReceive::EReceiveFlag_DeleteExisting
			)
			.f_Call()
		;

		CApplicationSettings StageSettings;
		StageSettings.m_RunAsUser = _Stage.m_RunAsUser;
		StageSettings.m_RunAsGroup = _Stage.m_RunAsGroup;
		StageSettings.m_bRunAsUserHasShell = _Stage.m_bRunAsUserHasShell;

		auto fFinishStage = [pThis, StageID = Result.m_StageID, StageSettings, Directory = Result.m_Directory](TCFuture<CFileTransferResult> _ReceiveFilesFuture) mutable -> TCFuture<void>
			{
				auto ReceiveResult = co_await fg_Move(_ReceiveFilesFuture).f_Wrap();

				auto *pStageReceive = pThis->mp_ApplicationStageReceives.f_FindEqual(StageID);
				if (!pStageReceive)
					co_return DMibErrorInstance("The application staging was aborted");

				auto Receive = fg_Move(pStageReceive->m_Receive);
				CStr StageDirectory = fg_Move(pStageReceive->m_StageDirectory);
				pThis->mp_ApplicationStageReceives.f_Remove(StageID);

				if (Receive)
					co_await fg_Move(Receive).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage receive");

				CExceptionPointer pStageException;
				if (!ReceiveResult)
					pStageException = DMibErrorInstance("Failed to receive the staged application files: {}"_f << ReceiveResult.f_GetExceptionStr()).f_ExceptionPointer();
				else
				{
					// The staged files belong to the user and group that the new version runs as
					auto BlockingActorCheckout = fg_BlockingActor();

					auto OwnerResult = co_await
						(
							g_Dispatch(BlockingActorCheckout) / [StageDirectory, StageSettings, Directory, pUniqueUserGroup = pThis->mp_pUniqueUserGroup]()
							{
								fsp_CreateApplicationUserGroup
									(
										StageSettings
										, [](CStr const &_Info)
										{
											DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment agent stage: {}", _Info);
										}
										, Directory / ".home"
										, pUniqueUserGroup
									)
								;

								CStr User = pUniqueUserGroup->f_GetUser(StageSettings.m_RunAsUser);
								CStr Group = pUniqueUserGroup->f_GetGroup(StageSettings.m_RunAsGroup);
								fsp_SetDirectoryOwnerSkippingLinks(StageDirectory, User, Group);
							}
						)
						.f_Wrap()
					;

					if (!OwnerResult)
					{
						pStageException = DMibErrorInstance("Failed to give the staged application files to the application user: {}"_f << OwnerResult.f_GetExceptionStr())
							.f_ExceptionPointer()
						;
					}
				}

				if (!pStageException)
					co_return {};

				// The abort subscription only deletes the directories of stages in mp_ApplicationStageReceives, and this stage was removed from it
				{
					auto BlockingActorCheckout = fg_BlockingActor();

					co_await
						(
							g_Dispatch(BlockingActorCheckout) / [StageDirectory]()
							{
								if (CFile::fs_FileExists(StageDirectory))
									CFile::fs_DeleteDirectoryRecursive(StageDirectory);
							}
						)
						.f_Wrap()
						> fg_LogError("Malterlib/Cloud/AppManager", "Failed to delete the failed application stage")
					;
				}

				co_return fg_Move(pStageException);
			}
		;

		TCFuture<void> FinishedFuture = pThis->self.f_Invoke(fg_Move(fFinishStage), fg_Move(ReceiveFilesFuture));

		Result.m_fFinish = g_ActorFunctor
			(
				g_ActorSubscription / [pThis, StageID = Result.m_StageID]() -> TCFuture<void>
				{
					auto *pStageReceive = pThis->mp_ApplicationStageReceives.f_FindEqual(StageID);
					if (!pStageReceive)
						co_return {};

					auto Receive = fg_Move(pStageReceive->m_Receive);
					CStr Name = fg_Move(pStageReceive->m_Name);
					CStr StageDirectory = fg_Move(pStageReceive->m_StageDirectory);
					pThis->mp_ApplicationStageReceives.f_Remove(StageID);

					DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Aborted staging of application '{}'", Name);

					if (Receive)
						co_await fg_Move(Receive).f_Destroy().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to destroy the application stage receive");

					{
						auto BlockingActorCheckout = fg_BlockingActor();

						co_await
							(
								g_Dispatch(BlockingActorCheckout) / [StageDirectory]()
								{
									if (CFile::fs_FileExists(StageDirectory))
										CFile::fs_DeleteDirectoryRecursive(StageDirectory);
								}
							)
							.f_Wrap()
							> fg_LogError("Malterlib/Cloud/AppManager", "Failed to delete the aborted application stage")
						;
					}

					co_return {};
				}
			)
			/ [Future = COptionalFuture{fg_Move(FinishedFuture)}, AllowDestroy = g_AllowWrongThreadDestroy]() mutable -> TCFuture<void>
			{
				return fg_Move(Future.m_Future);
			}
		;

		co_return fg_Move(Result);
	}

	// Reassigns the entries that have the directory's current owner or group
	void CAppManagerActor::fsp_ReassignApplicationFileOwnership(CStr const &_Directory, CStr const &_User, CStr const &_Group)
	{
		CStr OldUser = CFile::fs_GetOwner(_Directory);
		CStr OldGroup = CFile::fs_GetGroup(_Directory);

		bool bChangeUser = !_User.f_IsEmpty() && _User != OldUser;
		bool bChangeGroup = !_Group.f_IsEmpty() && _Group != OldGroup;

		if (!bChangeUser && !bChangeGroup)
			return;

		auto Paths = CFile::fs_FindFiles(_Directory / "*", EFileAttrib_Directory | EFileAttrib_File, true, false);
		Paths.f_Insert(_Directory);
		for (auto &Path : Paths)
		{
			// Setting the owner follows links, which can point outside the directory
			if (CFile::fs_GetAttributesOnLink(Path) & EFileAttrib_Link)
				continue;

			if (bChangeUser && CFile::fs_GetOwner(Path) == OldUser)
				CFile::fs_SetOwner(Path, _User);
			if (bChangeGroup && CFile::fs_GetGroup(Path) == OldGroup)
				CFile::fs_SetGroup(Path, _Group);
		}
	}

	// Only for the storage of a container or VM environment, which is the boundary: a link there, also one that replaces _Directory or a directory
	// in it while this runs, only reaches files of the environment. Links are skipped to keep the owner changes to the application's own files
	void CAppManagerActor::fsp_SetDirectoryOwnerSkippingLinks(CStr const &_Directory, CStr const &_User, CStr const &_Group)
	{
		auto Paths = CFile::fs_FindFiles(_Directory / "*", EFileAttrib_Directory | EFileAttrib_File, true, false);
		Paths.f_Insert(_Directory);
		for (auto &Path : Paths)
		{
			// Setting the owner follows links, which can point outside the directory
			if (CFile::fs_GetAttributesOnLink(Path) & EFileAttrib_Link)
				continue;

			if (!_User.f_IsEmpty())
				CFile::fs_SetOwner(Path, _User);
			if (!_Group.f_IsEmpty())
				CFile::fs_SetGroup(Path, _Group);
		}
	}

	void CAppManagerActor::fsp_ApplyStagedApplicationFiles
		(
			CAppManagerEnvironmentInterface::CApplicationApply const &_Apply
			, CStr const &_Directory
			, TCSharedPointer<CUniqueUserGroup> const &_pUniqueUserGroup
		)
	{
		CStr StageRoot = _Directory / "TempVersion";
		CStr StageDirectory = StageRoot / _Apply.m_StageID;

		if (!CFile::fs_FileExists(StageDirectory, EFileAttrib_Directory))
			DMibError(fg_Format("The application stage '{}' does not exist", _Apply.m_StageID));

		if (_Apply.m_bFailOnExisting && CFile::fs_FileExists(_Directory, EFileAttrib_Directory))
		{
			TCSet<CStr> AllowExist;
			AllowExist[_Directory + "/lost+found"];
			AllowExist[_Directory + "/.home"];
			AllowExist[_Directory + "/.tmp"];
			AllowExist[StageRoot];

			auto FoundFiles = CFile::fs_FindFiles(_Directory + "/*");
			for (auto &File : FoundFiles)
			{
				if (!AllowExist.f_FindEqual(File))
				{
					DMibError
						(
							"Files of an earlier application with this name exist at: '{}'. Add with --force-install to reuse them, or to delete them add with "
							"--force-install and remove with --delete-application-data"_f
							<< _Directory
						)
					;
				}
			}
		}

		// The stage belongs to the application account, whose update scripts can replace its directories with links to files of other accounts,
		// which the move would bring into the application directory
		for (auto &File : _Apply.m_Files)
		{
			if (fg_AppManager_HasLinkedParent(_Directory, "TempVersion/{}/{}"_f << _Apply.m_StageID << File))
				DMibError(fg_Format("The staged application file '{}' is below a link", File));
		}

		CApplicationSettings Settings;
		Settings.m_RunAsUser = _Apply.m_RunAsUser;
		Settings.m_RunAsGroup = _Apply.m_RunAsGroup;
		Settings.m_bRunAsUserHasShell = _Apply.m_bRunAsUserHasShell;

		fsp_CreateApplicationUserGroup
			(
				Settings
				, [](CStr const &_Info)
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment agent apply: {}", _Info);
				}
				, _Directory / ".home"
				, _pUniqueUserGroup
			)
		;

		for (auto &File : _Apply.m_OldFiles)
			fg_AppManager_DeleteApplicationFile(_Directory, File);

		CStr User = _pUniqueUserGroup->f_GetUser(Settings.m_RunAsUser);
		CStr Group = _pUniqueUserGroup->f_GetGroup(Settings.m_RunAsGroup);

		// The files the previous version left with the directory's owner or group, like databases, pass to a configured run as user or group
		// before the new files take over the directory
		if (CFile::fs_FileExists(_Directory, EFileAttrib_Directory))
			fsp_ReassignApplicationFileOwnership(_Directory, User, Group);

		auto fSetOwners = [&](CStr const &_Path)
			{
				CStr Path = _Path;
				while (Path.f_GetLen() >= _Directory.f_GetLen())
				{
					if (!Settings.m_RunAsUser.f_IsEmpty())
						CFile::fs_SetOwner(Path, User);
					if (!Settings.m_RunAsGroup.f_IsEmpty())
						CFile::fs_SetGroup(Path, Group);
					Path = CFile::fs_GetPath(Path);
				}
			}
		;

		TCVector<CStr> MovedFiles;
		for (auto &File : _Apply.m_Files)
		{
			CStr SourcePath = fg_Format("{}/{}", StageDirectory, File);
			CStr DestinationPath = fg_Format("{}/{}", _Directory, File);
			if (CFile::fs_FileExists(SourcePath, EFileAttrib_File | EFileAttrib_Link))
			{
				// The rename replaces a link at the destination itself
				fg_AppManager_CreateApplicationDirectory(_Directory, CFile::fs_GetPath(File));
				CFile::fs_AtomicReplaceFile(SourcePath, DestinationPath);
				MovedFiles.f_Insert(DestinationPath);
			}
			else if (CFile::fs_FileExists(SourcePath, EFileAttrib_Directory))
			{
				// Moving files only creates their parent directories, so each packaged directory, empty ones included, is created here with its normalized permissions
				fg_AppManager_CreateApplicationDirectory(_Directory, File);
				CFile::fs_SetAttributes(DestinationPath, (CFile::fs_GetAttributes(SourcePath) & ~EFileAttrib_Directory) | EFileAttrib_UnixAttributesValid);
				fsp_UpdateAttributes(DestinationPath);
				fSetOwners(DestinationPath);
			}
		}

		for (auto &DestinationPath : MovedFiles)
		{
			// The owner and mode changes follow links, whose targets can be outside the application or come with the OS
			// dependencies installed at launch
			if (!(CFile::fs_GetAttributesOnLink(DestinationPath) & EFileAttrib_Link))
			{
				if (!Settings.m_RunAsUser.f_IsEmpty())
					CFile::fs_SetOwner(DestinationPath, User);
				if (!Settings.m_RunAsGroup.f_IsEmpty())
					CFile::fs_SetGroup(DestinationPath, Group);

				fsp_UpdateAttributes(DestinationPath);
			}

			fSetOwners(CFile::fs_GetPath(DestinationPath));
		}

#ifdef DPlatformFamily_Linux
		if (!_Apply.m_VersionExtraInfo.f_IsEmpty())
		{
			try
			{
				CEJsonSorted ExtraInfo = CEJsonSorted::fs_FromString(_Apply.m_VersionExtraInfo);
				if (auto pLowPortExecutables = ExtraInfo.f_GetMember("LowPortExecutables", EJsonType_Array))
				{
					for (auto &Executable : pLowPortExecutables->f_Array())
					{
						if (!Executable.f_IsString())
							continue;

						CStr ExecutablePath = fg_AppManager_LowPortExecutablePath(_Directory, Executable.f_String());
						if (!ExecutablePath.f_IsEmpty())
							CProcessLaunch::fs_LaunchTool("setcap", {"cap_net_bind_service=ep", ExecutablePath});
					}
				}
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to set up low port executables: {}", _Exception);
			}
		}
#endif

		// The previous version can have replaced them with links to directories of other accounts
		fg_AppManager_CreateApplicationDirectory(_Directory, ".home");
		fg_AppManager_CreateApplicationDirectory(_Directory, ".tmp");
		fSetOwners(_Directory + "/.home");
		fSetOwners(_Directory + "/.tmp");

		CFile::fs_SetAttributes
			(
				_Directory
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

		CFile::fs_DeleteDirectoryRecursive(StageRoot);
	}

	TCFuture<void> CAppManagerActor::fp_WaitForApplicationFilesApply(CStr _Name)
	{
		while (auto *pWaits = mp_ApplyingApplicationFiles.f_FindEqual(_Name))
		{
			TCPromiseFuturePair<void> Finished;
			pWaits->f_Insert(fg_Move(Finished.m_Promise));
			co_await fg_Move(Finished.m_Future);
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_ApplyApplicationFiles(CApplicationApply _Apply)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (!CVersionManager::fs_IsValidApplicationName(_Apply.m_Name))
			co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Apply.m_Name);

		co_await pThis->fp_EnsureEnvironmentDataRoot();

		CStr Directory = pThis->fp_GetAgentApplicationDirectory(_Apply.m_Name);

		// The host stops waiting for an apply when it times out, but the apply goes on moving the files, so a retry or a discard of the stage
		// waits for it to finish
		co_await pThis->fp_WaitForApplicationFilesApply(_Apply.m_Name);

		pThis->mp_ApplyingApplicationFiles[_Apply.m_Name];
		auto EndApply = g_OnScopeExit / [pThis, Name = _Apply.m_Name]
			{
				auto *pWaits = pThis->mp_ApplyingApplicationFiles.f_FindEqual(Name);
				if (!pWaits)
					return;

				auto Waits = fg_Move(*pWaits);
				pThis->mp_ApplyingApplicationFiles.f_Remove(Name);

				for (auto &Wait : Waits)
					Wait.f_SetResult();
			}
		;

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [Apply = fg_Move(_Apply), Directory, pUniqueUserGroup = pThis->mp_pUniqueUserGroup]()
					{
						fsp_ApplyStagedApplicationFiles(Apply, Directory, pUniqueUserGroup);
					}
				)
				% "Failed to apply the staged application files"
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_DiscardApplicationStage(CStr _Name, CStr _StageID)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (!CVersionManager::fs_IsValidApplicationName(_Name))
			co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Name);

		if (pThis->mp_EnvironmentDataRoot.f_IsEmpty())
			co_return DMibErrorInstance("The environment has no managed application storage");

		bool bValidStageID = !_StageID.f_IsEmpty();
		for (ch8 const *pChar = _StageID.f_GetStr(); *pChar; ++pChar)
		{
			ch8 Char = *pChar;
			if (!((Char >= 'a' && Char <= 'z') || (Char >= 'A' && Char <= 'Z') || (Char >= '0' && Char <= '9')))
				bValidStageID = false;
		}

		if (!bValidStageID)
			co_return DMibErrorInstance("'{}' is not a valid application stage id"_f << _StageID);

		co_await pThis->fp_WaitForApplicationFilesApply(_Name);

		CStr StageRoot = pThis->fp_GetAgentApplicationDirectory(_Name) / "TempVersion";

		auto BlockingActorCheckout = fg_BlockingActor();

		// A later update can already have staged its files next to the discarded stage. The agent runs inside the environment, so the deletion
		// only reaches files of the environment, and the update scripts that could replace TempVersion with a link are trusted
		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [StageRoot, StageDirectory = StageRoot / _StageID]()
					{
						if (CFile::fs_FileExists(StageDirectory))
							CFile::fs_DeleteDirectoryRecursive(StageDirectory);

						if (CFile::fs_FileExists(StageRoot) && CFile::fs_FindFiles(StageRoot / "*").f_IsEmpty())
							CFile::fs_DeleteDirectory(StageRoot);
					}
				)
				% "Failed to discard the application stage"
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_RemoveApplicationStorage(CStr _Name)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		if (!CVersionManager::fs_IsValidApplicationName(_Name))
			co_return DMibErrorInstance("'{}' is not a valid application name"_f << _Name);

		co_await pThis->fp_EnsureEnvironmentDataRoot();

		co_await pThis->fp_WaitForApplicationFilesApply(_Name);

		CStr Directory = pThis->fp_GetAgentApplicationDirectory(_Name);

		auto BlockingActorCheckout = fg_BlockingActor();

		co_await
			(
				(
					g_Dispatch(BlockingActorCheckout) / [Directory]()
					{
						if (CFile::fs_FileExists(Directory))
							CFile::fs_DeleteDirectoryRecursive(Directory);
					}
				)
				% "Failed to remove the application storage"
			)
		;

		co_return {};
	}

	void CAppManagerActor::fp_ForwardApplicationStateChange(CAppManagerEnvironmentInterface::CApplicationStateChange _Change)
	{
		if (!mp_EnvironmentHostActor)
			return;

		mp_EnvironmentHostActor.f_CallActor(&CAppManagerEnvironmentHostInterface::f_ReportApplicationState)(fg_Move(_Change))
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to report application state to environment host")
		;
	}

	TCFuture<void> CAppManagerActor::fp_RegisterWithEnvironmentHost()
	{
		CStr EnvironmentHostID = mp_EnvironmentHostID;
		if (EnvironmentHostID.f_IsEmpty())
			co_return DMibErrorInstance("No environment host id provided in the deployment settings");

		CStr LaunchID = mp_Settings.m_InterfaceSettings.m_LaunchID;

		co_await mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_AllowHostsForNamespace
				, CStr(CAppManagerEnvironmentHostInterface::mc_pDefaultNamespace)
				, fg_CreateSet(EnvironmentHostID)
				, EDistributedActorTrustManagerOrderingFlag_None
			)
		;

		mp_EnvironmentHostActors = co_await mp_State.m_TrustManager->f_SubscribeTrustedActors<CAppManagerEnvironmentHostInterface>();

		co_await mp_EnvironmentHostActors.f_OnActor
			(
				g_ActorFunctor / [this, LaunchID](TCDistributedActor<CAppManagerEnvironmentHostInterface> _Host, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					mp_EnvironmentHostActor = _Host;

					NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> InterfaceWithID
						(
							mp_EnvironmentInterface.m_Actor->f_ShareInterface<CAppManagerEnvironmentInterface>()
							, g_ActorSubscription / []
							{
							}
						)
					;

					auto Registration = co_await _Host.f_CallActor(&CAppManagerEnvironmentHostInterface::f_RegisterEnvironmentAgent)
						(LaunchID, fg_Move(InterfaceWithID))
						.f_Timeout(60.0, "Timed out registering environment agent with host")
					;

					mp_EnvironmentHostRegistration = fg_Move(Registration);

					co_return {};
				}
				, g_ActorFunctor / [](TCWeakDistributedActor<CActor> _RemovedActor, CTrustedActorInfo _ActorInfo) -> TCFuture<void>
				{
					co_return {};
				}
				, "Malterlib/Cloud/AppManager"
				, "Failed to handle '{}' for environment host"
			)
		;

		co_return {};
	}

	// The returned subscription keeps the interface of the agent registered; releasing an older one leaves a newer registration in place
	auto CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_RegisterEnvironmentAgent
		(
			CStr _LaunchID
			, NConcurrency::TCDistributedActorInterfaceWithID<CAppManagerEnvironmentInterface> _Interface
		)
		-> TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();
		auto HostID = CallingHostInfo.f_GetRealHostID();

		TCSharedPointer<CEnvironment> pEnvironment;
		for (auto &pCandidate : pThis->mp_Environments)
		{
			bool bMatch = !pCandidate->m_LaunchID.f_IsEmpty() && pCandidate->m_LaunchID == _LaunchID;
			if (!bMatch)
				bMatch = !pCandidate->m_AgentHostID.f_IsEmpty() && pCandidate->m_AgentHostID == HostID;

			if (bMatch)
			{
				pEnvironment = pCandidate;
				break;
			}
		}

		if (!pEnvironment)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated environment agent tried to register: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
			co_return DErrorInstance("Environment agent not associated with your host");
		}

		pEnvironment->m_AgentHostID = HostID;

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Environment agent interface for '{}' registered from host '{}'"
				, pEnvironment->m_Name
				, CallingHostInfo.f_GetHostInfo().f_GetDesc()
			)
		;

		auto AssignSequence = ++pEnvironment->m_AgentInterfaceAssignSequence;

		co_await pThis->fp_OnEnvironmentAgentConnected(pEnvironment, fg_Move(_Interface));

		co_return g_ActorSubscription / [pThis, pEnvironment, AssignSequence]() -> TCFuture<void>
			{
				if (pEnvironment->m_bDeleted || pEnvironment->m_bStopping || AssignSequence != pEnvironment->m_AgentInterfaceAssignSequence)
					co_return {};

				pThis->fp_OnEnvironmentAgentDisconnected(pEnvironment);

				co_return {};
			}
		;
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_ReportApplicationState(CAppManagerEnvironmentInterface::CApplicationStateChange _Change)
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();

		auto pEnvironment = pThis->fp_EnvironmentFromHostID(CallingHostInfo.f_GetRealHostID());
		if (!pEnvironment)
			co_return DErrorInstance("Environment agent not associated with your host");

		_Change.m_Status = fg_AppManager_SanitizeGuestText(_Change.m_Status, 16 * 1024);
		pThis->fp_OnEnvironmentApplicationStateChange(pEnvironment, _Change);

		co_return {};
	}

	// Only the agent of the environment an application is launched in can request a ticket for it
	TCFuture<CStr> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_RequestApplicationConnectionTicket(CStr _LaunchID)
	{
		auto pThis = m_pThis;

		CCallingHostInfo CallingHostInfo = fg_GetCallingHostInfo();

		auto pEnvironment = pThis->fp_EnvironmentFromHostID(CallingHostInfo.f_GetRealHostID());
		if (!pEnvironment)
			co_return DErrorInstance("Environment agent not associated with your host");

		if (_LaunchID.f_IsEmpty())
			co_return DErrorInstance("A launch id is required to request an application connection ticket");

		auto pApplication = pThis->fp_ApplicationFromLaunchID(_LaunchID);
		if (!pApplication || pApplication->m_pLaunchedEnvironment != pEnvironment)
			co_return DErrorInstance("No application with the launch id is launched in your environment");

		// The ticket fields belong to the current launch, whose ticket a request for an ended launch must not revoke
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (pApplication->m_bDeleted)
						return DErrorInstance("Application deleted");

					if (pApplication->m_pLaunchedEnvironment != pEnvironment || pApplication->m_LaunchID != _LaunchID)
						return DErrorInstance("The application stopped while its connection ticket was requested");

					return {};
				}
			)
		;

		auto Address = co_await pThis->fp_EnsureEnvironmentListen(pEnvironment);

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Generating connection ticket for '{}' in environment '{}'", pApplication->m_Name, pEnvironment->m_Name);

		CStr NotificationID = NCryptography::fg_RandomID();

		auto Ticket = co_await pThis->mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_GenerateConnectionTicket
				, Address
				, g_ActorFunctor
				(
					g_ActorSubscription / [NotificationID, pApplication]
					{
						if (pApplication->m_EnvironmentTicketID == NotificationID)
						{
							pApplication->m_EnvironmentTicketID.f_Clear();
							pApplication->m_EnvironmentTicketSubscription = {};
						}
					}
				)
				/ [pThis, NotificationID, pApplication, pEnvironment, _LaunchID](CStr _HostID, CCallingHostInfo _HostInfo, CByteVector _CertificateRequest) -> TCFuture<void>
				{
					if (pApplication->m_EnvironmentTicketID == NotificationID)
					{
						pApplication->m_EnvironmentTicketID.f_Clear();
						pApplication->m_EnvironmentTicketSubscription = {};
					}

					if (pApplication->m_bDeleted)
						co_return DMibErrorInstance("Application deleted");

					// The ticket only identifies the application for the launch it was issued for
					if (pApplication->m_pLaunchedEnvironment != pEnvironment || pApplication->m_LaunchID != _LaunchID)
						co_return DMibErrorInstance("The launch the connection ticket was issued for has ended");

					if (pApplication->m_AssociatedHostID != _HostID)
					{
						pApplication->m_AssociatedHostID = _HostID;
						pThis->fp_RebootPrevention_UpdateApplicationFlags(pApplication->m_Name);
						pThis->fp_SendAppChange_AddedOrChanged(*pApplication);
					}

					auto Result = co_await pThis->fp_UpdateApplicationJson(pApplication).f_Wrap();

					if (!Result)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Info
								, "Failed to update application JSON when granting connection ticket for '{}': {}"
								, pApplication->m_Name
								, Result.f_GetExceptionStr()
							)
						;
						co_return DMibErrorInstance("Failed to update application JSON, see AppManager log for details");
					}

					co_return {};
				}
				, nullptr
			)
		;

		pApplication->m_EnvironmentTicketID = NotificationID;
		pApplication->m_EnvironmentTicketSubscription = fg_Move(Ticket.m_NotificationsSubscription);

		co_return Ticket.m_Ticket.f_ToStringTicket();
	}

	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_ConfigureAgent(CAgentConfig _Config)
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

		// The host monitor uses the OS dependencies of the agent application but runs without them, so their failure fails nothing and the next
		// configuration retries them
		if (!_Config.m_AgentOSDependencies.f_IsEmpty())
		{
			CStr Fingerprint = fsp_GetOSDependenciesFingerprint(_Config.m_AgentOSDependencies);
			if (!pThis->mp_InstalledOSDependenciesFingerprints.f_FindEqual(Fingerprint))
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Installing the agent OS dependencies");

				auto Result = co_await pThis->fp_InstallOSDependencies(fg_Move(_Config.m_AgentOSDependencies)).f_Wrap();
				if (Result)
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Installed the agent OS dependencies");
				else
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to install the agent OS dependencies, which the host monitor uses: {}", Result.f_GetExceptionStr());
			}
		}

		CStr DependencyError;
		if (!_Config.m_OSDependencies.f_IsEmpty())
		{
			auto Result = co_await pThis->fp_InstallOSDependencies(fg_Move(_Config.m_OSDependencies)).f_Wrap();
			if (!Result)
				DependencyError = "Failed to install the environment OS dependencies: {}"_f << Result.f_GetExceptionStr();
		}

		// The applications don't need the host monitor, so its failure fails nothing; the next configuration sets it up again
		if (auto Result = co_await pThis->fp_ConfigureHostMonitorFromHost(fg_Move(_Config.m_AutoUpdateConfig)).f_Wrap(); !Result)
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to configure the host monitor of the environment agent: {}", Result.f_GetExceptionStr());

		// A reconnected host has lost the update state, and configures the agent after every connect
		if (pThis->mp_bEnvironmentPatching || pThis->mp_nEnvironmentOSDependencyInstalls)
			co_await pThis->fp_ReportEnvironmentBusyToHost().f_Wrap() > fg_LogError("Malterlib/Cloud/AppManager", "Failed to report the environment update state");

		// The host holds back the launches it dispatches to the environment on the failure
		if (!DependencyError.f_IsEmpty())
			co_return DMibErrorInstance(DependencyError);

		co_return {};
	}

#ifndef DPlatformFamily_Windows
	namespace
	{
		// The delay gives the shutdown reply time to reach the host
		TCFuture<void> fg_ShutdownEnvironmentAfterDelay()
		{
			co_await fg_Timeout(2.0);

			auto BlockingActorCheckout = fg_BlockingActor();

			auto Result = co_await
				(
					g_Dispatch(BlockingActorCheckout) / []()
					{
						CStr StdOut;
						CStr StdErr;
						uint32 ExitCode = 0;
						if (!CProcessLaunch::fs_LaunchBlock("/sbin/shutdown", fg_CreateVector<CStr>("-h", "now"), StdOut, StdErr, ExitCode))
							DMibError(fg_Format("Failed to launch shutdown: {}", StdErr));

						if (ExitCode != 0)
							DMibError(fg_Format("Shutting down failed with status {}: {}", ExitCode, fg_ConcatOutput(StdOut, StdErr)));
					}
				)
				.f_Wrap()
			;

			if (!Result)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Failed to shut down the environment: {}", Result.f_GetExceptionStr());

			co_return {};
		}
	}
#endif

	// Schedules the power-off of the system the agent runs in without waiting for it, so the call can reply; the host calls it only for VMs
	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentInterfaceImplementation::f_ShutdownEnvironment()
	{
		auto pThis = m_pThis;

		if (!pThis->mp_bEnvironmentAgent)
			co_return DMibErrorInstance("Not running as an environment agent");

#ifdef DPlatformFamily_Windows
		co_return DMibErrorInstance("Shutting down the environment from inside is not supported on this platform");
#else
		fg_ShutdownEnvironmentAfterDelay().f_DiscardResult();

		co_return {};
#endif
	}

	void CAppManagerActor::fs_ApplyEnvironmentHostName(CStr const &_HostName)
	{
#ifdef DPlatformFamily_Linux
		// Local agents get no host name, since they share the host's UTS namespace. There is no platform abstraction for setting the host name
		if (NProcess::NPlatform::fg_Process_GetComputerName() != _HostName)
		{
			if (sethostname(_HostName.f_GetStr(), _HostName.f_GetLen()) != 0)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to set the environment host name to '{}': {}", _HostName, NPlatform::fg_FormatErrno(errno));
				return;
			}

			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Set the environment host name to '{}'", _HostName);
		}

		try
		{
			// Persist the name for processes that read it from the configuration and
			// across virtual machine reboots
			CStr HostNameFile = "/etc/hostname";
			CStr HostNameLine = _HostName + "\n";
			if (!CFile::fs_FileExists(HostNameFile) || CFile::fs_ReadStringFromFile(HostNameFile, true) != HostNameLine)
				CFile::fs_WriteStringToFile(HostNameFile, HostNameLine, false);

			// Keep the host name resolvable for tools that look up their own name. The bytes of the file are kept, and the check and the appended
			// entry assume an ASCII-compatible encoding
			CStr HostsFile = "/etc/hosts";
			CByteVector HostsBytes;
			if (CFile::fs_FileExists(HostsFile))
				HostsBytes = CFile::fs_ReadFile(HostsFile);

			CStr Hosts(reinterpret_cast<ch8 const *>(HostsBytes.f_GetArray()), HostsBytes.f_GetLen());
			if (!fg_AppManager_HostsFileHasHostName(Hosts, _HostName))
			{
				CStr Entry;
				if (!Hosts.f_IsEmpty() && !Hosts.f_EndsWith("\n"))
					Entry += "\n";
				Entry += "127.0.0.1\t" + _HostName + "\n";
				HostsBytes.f_Insert(reinterpret_cast<uint8 const *>(Entry.f_GetStr()), Entry.f_GetLen());
				CFile::fs_WriteFile(HostsFile, HostsBytes);
			}
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to persist the environment host name '{}': {}", _HostName, _Exception);
		}
#elif defined(DPlatformFamily_macOS)
		bool bRenamed = NProcess::NPlatform::fg_Process_GetComputerName() != _HostName;

		// macOS guests keep the names in the system configuration, which persists them across reboots
		for (auto *pName : {"HostName", "LocalHostName", "ComputerName"})
		{
			try
			{
				CProcessLaunch::fs_LaunchTool("/usr/sbin/scutil", {"--set", pName, _HostName});
			}
			catch (CException const &_Exception)
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "Failed to set the environment {} to '{}': {}", pName, _HostName, _Exception);
			}
		}

		// configd changes the kernel host name after scutil returns, and the distributed identity reads it once, before it connects
		if (bRenamed)
		{
			CStopwatch Stopwatch{true};
			while (NProcess::NPlatform::fg_Process_GetComputerName() != _HostName && Stopwatch.f_GetTime() < 10.0)
				NSys::fg_Thread_Sleep(0.1);

			if (NProcess::NPlatform::fg_Process_GetComputerName() != _HostName)
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Warning, "The host name did not change to '{}' within 10 seconds", _HostName);
		}
#endif
	}

	// An update in progress blocks host reboots until it is reported finished, the agent disconnects or the deferral limit passes. Refuses updates
	// that start once the environment stops, the host reboots or the AppManager shuts down, which have already waited for the updates
	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_ReportEnvironmentUpdateState(bool _bUpdating, CStr _Description)
	{
		auto pThis = m_pThis;

		auto pEnvironment = pThis->fp_EnvironmentFromHostID(fg_GetCallingHostInfo().f_GetRealHostID());
		if (!pEnvironment)
			co_return DErrorInstance("Environment agent not associated with your host");

		// A stop that waits for the start to give up hasn't set m_bStopping or closed the admission yet
		if
		(
			_bUpdating
			&& pEnvironment->m_AgentUpdateInProgress.f_IsEmpty()
			&& (pEnvironment->m_bStopping || pEnvironment->m_bStartAbortRequested || pEnvironment->m_nUpdateAdmissionClosed || pThis->mp_bRebooting || pThis->mp_bStoppingApp)
		)
		{
			co_return DErrorInstance("The environment is stopping");
		}

		CStr Description = fg_AppManager_SanitizeGuestText(_Description, 256);
		CStr PreviousState = pEnvironment->m_AgentUpdateInProgress;
		if (_bUpdating)
			pEnvironment->f_SetAgentUpdateInProgress(Description.f_IsEmpty() ? CStr("Installing updates") : fg_Move(Description));
		else
			pEnvironment->f_SetAgentUpdateInProgress({});

		// The guest reports the state as often as it likes, so only changes reach the log of the host
		if (pEnvironment->m_AgentUpdateInProgress == PreviousState)
			co_return {};

		DMibLogWithCategory
			(
				Malterlib/Cloud/AppManager
				, Info
				, "Environment '{}' update state: {}"
				, pEnvironment->m_Name
				, _bUpdating ? pEnvironment->m_AgentUpdateInProgress : CStr("Finished")
			)
		;

		co_return {};
	}

	// Schedules a restart for when the environment is idle; the reply does not mean it restarted. Like every stop of an application, the restart
	// first stops the applications that depend on the applications of the environment, wherever they run: the administrator who configured those
	// dependencies ties the dependents to the restarts the environment needs for its updates
	TCFuture<void> CAppManagerActor::CAppManagerEnvironmentHostInterfaceImplementation::f_RequestEnvironmentRestart(CStr _Reason)
	{
		auto pThis = m_pThis;

		auto pEnvironment = pThis->fp_EnvironmentFromHostID(fg_GetCallingHostInfo().f_GetRealHostID());
		if (!pEnvironment)
			co_return DErrorInstance("Environment agent not associated with your host");

		// The guest asks for restarts itself, so the host enforces its own setting, which can also have changed since the agent was configured
		if (!fg_AppManager_EnvironmentAgentAutomaticReboot(pEnvironment->m_Settings.m_Type, pEnvironment->m_Settings.m_bContainerReadOnly, pThis->mp_State.m_ConfigDatabase.m_Data))
			co_return DErrorInstance("Automatic restarts are not enabled for this environment");

		// The requests that come while a restart waits join it, so only the first one reaches the log of the host
		if (!pEnvironment->m_bRestartWhenIdlePending)
			DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Environment '{}' requested a restart: {}", pEnvironment->m_Name, fg_AppManager_SanitizeGuestText(_Reason, 256));

		// Not awaited: the restart stops the agent making this call
		fg_CallSafe
			(
				TCFunctionMovable<TCFuture<void> ()>
				(
					[pThis, pEnvironment]() -> TCFuture<void>
					{
						co_return co_await pThis->fp_RestartEnvironmentWhenIdle(pEnvironment);
					}
				)
			)
			> fg_LogError("Malterlib/Cloud/AppManager", "Failed to restart environment after agent request")
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_ReportEnvironmentUpdateStateToHost(bool _bUpdating)
	{
		if (!mp_EnvironmentHostActor)
			co_return {};

		co_return co_await mp_EnvironmentHostActor.f_CallActor(&CAppManagerEnvironmentHostInterface::f_ReportEnvironmentUpdateState)
			(_bUpdating, CStr("Installing OS updates"))
			.f_Timeout(60.0, "Timed out reporting the environment update state")
		;
	}

	TCFuture<void> CAppManagerActor::fp_ReportEnvironmentBusyToHost()
	{
		co_return co_await fp_ReportEnvironmentUpdateStateToHost(mp_bEnvironmentPatching || mp_nEnvironmentOSDependencyInstalls);
	}

	TCFuture<void> CAppManagerActor::fp_RequestEnvironmentRestartFromHost()
	{
		if (!mp_EnvironmentHostActor)
			co_return DMibErrorInstance("Not connected to the environment host");

		DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Requesting an environment restart to finish OS updates");

		co_return co_await mp_EnvironmentHostActor.f_CallActor(&CAppManagerEnvironmentHostInterface::f_RequestEnvironmentRestart)
			(CStr("OS updates require a restart"))
			.f_Timeout(60.0, "Timed out requesting the environment restart")
		;
	}
}
