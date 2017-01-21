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
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_GetSwiftBaseURL(CGetSwiftBaseURL &&_Params) -> TCContinuation<CGetSwiftBaseURL::CResult>
	{
		auto &CallingHostInfo = fg_GetCallingHostInfo();
		auto pThis = m_pThis;
		
		TCContinuation<CCloudAPIManager::CGetSwiftBaseURL::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return fsp_LogActivityError(CallingHostInfo, "Cloud context format not valid", nullptr);
		
		if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostInfo.f_GetRealHostID(), "ObjectStorage/GetSwiftBaseURLAll", fg_Format("ObjectStorage/GetSwiftBaseURL/{}", _Params.m_CloudContext)))
			return pThis->fp_AccessDenied(CallingHostInfo, "Get Swift base URL");
		
		auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			return fsp_LogActivityError(CallingHostInfo,  fg_Format("No such cloud context: {}", _Params.m_CloudContext), nullptr);
		
		pThis->fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [Continuation, _Params](COpenStackServiceInfo &&_ServiceInfo)
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

