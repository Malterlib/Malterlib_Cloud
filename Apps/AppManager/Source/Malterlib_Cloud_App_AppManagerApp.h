// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#pragma once

#include <Mib/Concurrency/DistributedApp>

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

	/// Where the agent of a VM environment reaches the environment listen of the host
	struct CAppManagerEnvironmentSocketDirectory
	{
		NStr::CStr m_SharedDirectory; /// The AppManager's directory of the environment directories; empty in the temporary directory
		NStr::CStr m_SocketDirectory;
		NStr::CStr m_SocketPath;
	};

	auto fg_AppManager_EnvironmentSocketDirectory(NStr::CStr const &_RootDirectory, NStr::CStr const &_EnvironmentName, bool _bRunningAsRoot) -> CAppManagerEnvironmentSocketDirectory;
	void fg_AppManager_PrepareEnvironmentSocketDirectory(CAppManagerEnvironmentSocketDirectory const &_Directory);

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
#ifdef DPlatformFamily_Linux
	NStr::CStr fg_AppManager_LowPortExecutableDigest(NStr::CStr const &_Path);
	NStr::CStr fg_AppManager_InstalledLowPortExecutablePath(NStr::CStr const &_Directory, NStr::CStr const &_Executable, NStr::CStr const &_InstalledDigest);
#endif
}
