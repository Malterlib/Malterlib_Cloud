// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Network/SSL>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCContinuation<CCloudAPIManager::CGetSwiftBaseURL::CResult> CCloudAPIManagerDaemonActor::CServer::fp_Protocol_GetSwiftBaseURL
		(
			CCallingHostInfo const &_CallingHostInfo
			, CCloudAPIManager::CGetSwiftBaseURL &&_Params
		)
	{
		TCContinuation<CCloudAPIManager::CGetSwiftBaseURL::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return fsp_LogActivityError(_CallingHostInfo, "Cloud context format not valid", nullptr);
		
		if (!mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "ObjectStorage/GetSwiftBaseURLAll", fg_Format("ObjectStorage/GetSwiftBaseURL/{}", _Params.m_CloudContext)))
			return fp_AccessDenied(_CallingHostInfo, "Get Swift base URL");
		
		auto *pCloudContext = mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			return fsp_LogActivityError(_CallingHostInfo,  fg_Format("No such cloud context: {}", _Params.m_CloudContext), nullptr);
		
		fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [this, Continuation, _Params, _CallingHostInfo](COpenStackServiceInfo &&_ServiceInfo)
			{
				if (!_ServiceInfo.m_URLs.f_Exists("swift"))
				{
					Continuation.f_SetException(DMibImpExceptionInstance(CExceptionCloudAPI, "Swift service not available"));
					return;
				}

				CCloudAPIManager::CGetSwiftBaseURL::CResult Result;
				Result.m_BaseURL = _ServiceInfo.m_URLs["swift"];
				Continuation.f_SetResult(Result);
			}
		;
		
		return Continuation;
	}
}

