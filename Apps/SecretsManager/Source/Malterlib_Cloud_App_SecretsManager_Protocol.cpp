// Copyright © Unbroken AB
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	TCFuture<void> CSecretsManagerDaemonActor::CServer::fp_Publish()
	{
		auto Result = co_await mp_ProtocolInterface.f_Publish<CSecretsManager>(mp_AppState.m_DistributionManager, this).f_Wrap();
		if (!Result)
			DMibLog(Error, "Failed to publish secrets manager {}", Result.f_GetExceptionStr());

		co_return {};
	}
}
