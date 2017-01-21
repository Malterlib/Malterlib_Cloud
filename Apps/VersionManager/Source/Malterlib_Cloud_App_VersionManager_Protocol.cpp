// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	void CVersionManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolInterface.f_Publish<CVersionManager>(mp_AppState.m_DistributionManager, this, "com.malterlib/Cloud/VersionManager");
	}

	NException::CException CVersionManagerDaemonActor::CServer::fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description, CStr const &_UserDescription)
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
