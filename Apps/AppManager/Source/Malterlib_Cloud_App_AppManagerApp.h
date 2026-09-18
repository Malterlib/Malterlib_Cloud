// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Concurrency/DistributedApp>

namespace NMib::NCloud
{
	NConcurrency::TCActor<NConcurrency::CDistributedAppActor> fg_ConstructApp_AppManager();

	COnScopeExitShared fg_AppManager_RegisterInProcessFactory
		(
			NStr::CStr const &_ExecutablePath
			, NFunction::TCFunction<NConcurrency::TCActor<NConcurrency::CDistributedAppActor> ()> &&_fDistributedAppFactory
		)
	;

	/// Description of a container run command for an environment agent
	struct CAppManagerContainerLaunch
	{
		NStr::CStr m_ContainerName;
		NStr::CStr m_Image;
		NStr::CStr m_Network;
		uint64 m_MemoryLimitMB = 0;
		fp64 m_CPULimit = 0.0;
		bool m_bReadOnly = false; /// Read-only container filesystem; all writes must go to mounted paths
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_Mounts; /// Host path to path inside the container
		NContainer::TCVector<NStr::CStr> m_ExtraArguments;
		NContainer::TCMap<NStr::CStr, NStr::CStr> m_AddHosts; /// Hostname to address, for example host-gateway
		NContainer::TCVector<NStr::CStr> m_PassEnvironment; /// Environment variables forwarded from the client environment into the container
		NStr::CStr m_WorkingDirectory;
		NStr::CStr m_Executable; /// Path inside the container
		NContainer::TCVector<NStr::CStr> m_Parameters;
	};

	NContainer::TCVector<NStr::CStr> fg_AppManager_BuildContainerRunArguments(CAppManagerContainerLaunch const &_Launch);

	/// Identity of the operating system the AppManager runs on, used to select OS dependencies
	struct CAppManagerOSIdentity
	{
		NStr::CStr m_ID; /// Lower case os-release ID on Linux, "macos" or "windows"
		NContainer::TCVector<NStr::CStr> m_Like; /// Lower case os-release ID_LIKE entries on Linux
	};

	CAppManagerOSIdentity fg_AppManager_ParseOSRelease(NStr::CStr const &_Contents);
	CAppManagerOSIdentity fg_AppManager_GetOSIdentity();

	/// Builds a bash script installing the OS dependencies matching the identity with the
	/// platform package manager. Returns an empty script when nothing matches; sets o_Error
	/// when dependencies match but cannot be installed (unknown package manager or invalid
	/// package names).
	NStr::CStr fg_AppManager_BuildOSDependencyInstallScript
		(
			CAppManagerOSIdentity const &_Identity
			, NContainer::TCMap<NStr::CStr, NContainer::TCVector<NStr::CStr>> const &_Dependencies
			, NStr::CStr &o_Error
		)
	;
}
