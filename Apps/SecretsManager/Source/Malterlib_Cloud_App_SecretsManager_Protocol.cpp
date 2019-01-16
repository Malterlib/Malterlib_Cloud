// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	void CSecretsManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CSecretsManager>(mp_AppState.m_DistributionManager, this, "com.malterlib/Cloud/SecretsManager") > [](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
					DMibLog(Error, "Failed to publish secrets manager {}", _Result.f_GetExceptionStr());
			}
		;
	}
}
