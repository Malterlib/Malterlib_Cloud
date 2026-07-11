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
