// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Concurrency/DistributedApp>
#include <Mib/Cloud/AppManager>

namespace NMib::NCloud
{
	NConcurrency::TCActor<NConcurrency::CDistributedAppActor> fg_ConstructApp_AppManager();

	auto fg_AppManager_RegisterInProcessFactory
		(
			NStr::CStr const &_ExecutablePath
			, NFunction::TCFunction<NConcurrency::TCActor<NConcurrency::CDistributedAppActor> ()> &&_fDistributedAppFactory
		)
		-> COnScopeExitShared
	;

	bool fg_AppManager_IsValidVMProvisionUsername(NStr::CStr const &_Username);

	struct CAppManagerContainerLaunch
	{
		NStr::CStr m_ContainerName;
		NStr::CStr m_Hostname;                                      /// Host name inside the container; empty keeps the runtime default
		NStr::CStr m_Image;
		NStr::CStr m_Network;
		uint64 m_MemoryLimitMB = 0;
		fp64 m_CPULimit = 0.0;
		bool m_bReadOnly = false;                                   /// Read-only root filesystem with a writable /tmp tmpfs
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_Mounts;         /// Host path or runtime volume name to path inside the container
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_ReadOnlyMounts; /// Host path to path inside the container, mounted read-only
		bool m_bAppleContainer = false;                             /// Selects the read-only mount syntax of the Apple container runtime
		NContainer::TCVector<NStr::CStr> m_ExtraArguments;
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_AddHosts;       /// Hostname to address, for example host-gateway
		NContainer::TCVector<NStr::CStr> m_PassEnvironment;         /// NAME forwards a variable from the client environment, NAME=value sets it
		NStr::CStr m_WorkingDirectory;
		NStr::CStr m_Executable;                                    /// Path inside the container
		NContainer::TCVector<NStr::CStr> m_Parameters;
	};

	NContainer::TCVector<NStr::CStr> fg_AppManager_BuildContainerRunArguments(CAppManagerContainerLaunch const &_Launch);
	bool fg_AppManager_ContainerErrorReportsMissing(NStr::CStr const &_RuntimeExecutable, NStr::CStr const &_ErrorOutput);
	bool fg_AppManager_ContainerVolumeErrorReportsMissing(NStr::CStr const &_RuntimeExecutable, NStr::CStr const &_ErrorOutput);

	/// Limits how long a guest of a container or VM environment defers host reboots and environment stops and restarts. A deferral that starts within
	/// mc_LimitSeconds of the end of the previous one continues it, so a guest cannot renew it by ending and starting deferrals. The times are monotonic
	/// seconds from fs_Now, which changes of the system time don't move
	struct CAppManagerGuestDeferral
	{
		void f_Update(bool _bActive, fp64 _Now);
		bool f_Defers(fp64 _Now) const;

		static fp64 fs_Now();

		static constexpr fp64 mc_LimitSeconds = 60.0 * 60.0;

		fp64 m_Start = 0.0;
		fp64 m_Finished = 0.0;
		bool m_bActive = false;
		bool m_bFinished = false;
	};

	NStr::CStr fg_AppManager_EnvironmentAgentAutoUpdateConfig(CAppManagerInterface::EEnvironmentType _Type, bool _bReadOnly, NEncoding::CEJsonSortedYaml const &_HostConfig);
	NStr::CStr fg_AppManager_SanitizeGuestText(NStr::CStr const &_Text, umint _MaxLength);

	/// Where the agent of a docker or VM environment reaches the environment listen of the host
	struct CAppManagerEnvironmentSocketDirectory
	{
		NStr::CStr m_SharedDirectory; /// The AppManager's directory of the environment directories; empty in the temporary directory
		NStr::CStr m_EnvironmentDirectory;
		NStr::CStr m_SocketDirectory; /// The environment directory, or the directory in it that a container mounts
		NStr::CStr m_SocketPath;
	};

	auto fg_AppManager_EnvironmentSocketDirectory(NStr::CStr const &_RootDirectory, NStr::CStr const &_EnvironmentName, bool _bContainer, bool _bRunningAsRoot)
		-> CAppManagerEnvironmentSocketDirectory
	;
	void fg_AppManager_PrepareEnvironmentSocketDirectory(CAppManagerEnvironmentSocketDirectory const &_Directory);

	bool fg_AppManager_HostsFileHasHostName(NStr::CStr const &_Hosts, NStr::CStr const &_HostName);
	NStr::CStr fg_AppManager_EnvironmentHostName(NStr::CStr const &_ComputerName, NStr::CStr const &_EnvironmentName);
	bool fg_AppManager_ContainerSharesHostName(NContainer::TCVector<NStr::CStr> const &_ExtraArguments, NStr::CStr const &_Network);

	/// Identity of the operating system the AppManager runs on, used to select OS dependencies
	struct CAppManagerOSIdentity
	{
		NStr::CStr m_ID;                         /// Lower case os-release ID on Linux ("linux" without one), "macos" or "windows"
		NContainer::TCVector<NStr::CStr> m_Like; /// Lower case os-release ID_LIKE entries on Linux
	};

	CAppManagerOSIdentity fg_AppManager_ParseOSRelease(NStr::CStr const &_Contents);
	CAppManagerOSIdentity fg_AppManager_GetOSIdentity();

	auto fg_AppManager_BuildOSDependencyInstallScript
		(
			CAppManagerOSIdentity const &_Identity
			, NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> const &_Dependencies
			, NStr::CStr &o_Error
		)
		-> NStr::CStr
	;

	bool fg_AppManager_HasLinkedParent(NStr::CStr const &_Directory, NStr::CStr const &_File);
	void fg_AppManager_DeleteApplicationFile(NStr::CStr const &_Directory, NStr::CStr const &_File);
	void fg_AppManager_CreateApplicationDirectory(NStr::CStr const &_Directory, NStr::CStr const &_RelativeDirectory);
	NStr::CStr fg_AppManager_LowPortExecutableDigest(NStr::CStr const &_Path);
	NStr::CStr fg_AppManager_InstalledLowPortExecutablePath(NStr::CStr const &_Directory, NStr::CStr const &_Executable, NStr::CStr const &_InstalledDigest);
	auto fg_AppManager_ReplaceApplicationFiles
		(
			NStr::CStr const &_Directory
			, NStr::CStr const &_SourceDirectory
			, NContainer::TCVector<NStr::CStr> const &_OldFiles
			, NContainer::TCVector<NStr::CStr> const &_Files
		)
		-> NContainer::TCVector<NStr::CStr>
	;
}
