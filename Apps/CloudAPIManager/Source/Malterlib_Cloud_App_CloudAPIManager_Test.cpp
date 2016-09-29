// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCContinuation<CDistributedAppCommandLineResults> CCloudAPIManagerDaemonActor::CServer::f_CommandLine_TestCloudAPI(CEJSON const &_Params)
	{
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		CCallingHostInfo DummyHostInfo{mp_AppState.m_DistributionManager, "TestHostID", {}, {}, 0x101};
		CCloudAPIManager::CEnsureContainer EnsureContainer;
		EnsureContainer.m_CloudContext = "TestContext";
		fp_Protocol_EnsureContainer
			(
				DummyHostInfo
				, fg_Move(EnsureContainer)
			)
			> Continuation / [this, Continuation](CCloudAPIManager::CEnsureContainer::CResult &&_Result)
			{
				CDistributedAppCommandLineResults CommandResult;
				CommandResult.f_AddStdErr("Test finished\n");
				Continuation.f_SetResult(CommandResult);
			}
		;
		
		return Continuation;
	}
}
