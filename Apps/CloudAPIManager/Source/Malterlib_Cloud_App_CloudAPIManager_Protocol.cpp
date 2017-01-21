// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	void CCloudAPIManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CCloudAPIManager>(mp_AppState.m_DistributionManager, this, "com.malterlib/Cloud/CloudAPIManager");
	}

	NException::CException CCloudAPIManagerDaemonActor::CServer::fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description, CStr const &_UserDescription)
	{
		if (!_UserDescription.f_IsEmpty())
		{
			fsp_LogActivityWarning(_CallingHostInfo, fg_Format("Denied access to: {} '{}'", _Description, _UserDescription));
			return DMibErrorInstance(_UserDescription);
		}
		else
		{
			fsp_LogActivityWarning(_CallingHostInfo, fg_Format("Denied access to: {}", _Description));
			return DMibErrorInstance("Access denied");
		}
	}
}
