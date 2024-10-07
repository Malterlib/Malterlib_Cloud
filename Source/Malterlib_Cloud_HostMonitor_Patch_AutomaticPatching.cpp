// Copyright © 2024 Favro Holding AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_HostMonitor.h"
#include "Malterlib_Cloud_HostMonitor_Internal.h"

#include <Mib/Concurrency/AsyncDestroy>
#include <Mib/Process/ProcessLaunchActor>

namespace NMib::NCloud
{
	using namespace NProcess;

	TCFuture<void> fg_RunApt(CStr _Application, TCVector<CStr> _Params)
	{
		TCActor<CProcessLaunchActor> LaunchActor(fg_Construct());
		auto AutoDestroy = co_await fg_AsyncDestroy(LaunchActor);

		CProcessLaunchActor::CSimpleLaunch Launch
			{
				_Application
				, _Params
				, CFile::fs_GetProgramDirectory()
				, CProcessLaunchActor::ESimpleLaunchFlag_GenerateExceptionOnNonZeroExitCode
			}
		;

		Launch.m_ToLog = CProcessLaunchActor::ELogFlag_Error | CProcessLaunchActor::ELogFlag_StdOut | CProcessLaunchActor::ELogFlag_StdErr;
		Launch.m_Params.m_Environment["DEBIAN_FRONTEND"] = "noninteractive";
		Launch.m_Params.m_Environment["NEEDRESTART_MODE"] = "l";

		co_await LaunchActor(&CProcessLaunchActor::f_LaunchSimple, fg_Move(Launch));

		co_return {};
	}

	TCFuture<void> CHostMonitor::CInternal::f_Patch_InstallPatches()
	{
#ifndef DPlatformFamily_Linux
		co_return {};
#else
		co_await fg_RunApt("apt-get", {"update"});
		co_await fg_RunApt("apt-get", {"dist-upgrade", "--yes"});
		co_await fg_RunApt("apt-get", {"autoremove", "--yes"});

		co_return {};
#endif
	}
}
