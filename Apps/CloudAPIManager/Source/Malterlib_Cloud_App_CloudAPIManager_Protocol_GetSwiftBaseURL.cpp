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
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		TCContinuation<CCloudAPIManager::CGetSwiftBaseURL::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return Auditor.f_Exception("Cloud context format not valid");
		
		pThis->mp_Permissions.f_HasPermission("Get swift base URL", {"ObjectStorage/GetSwiftBaseURLAll", fg_Format("ObjectStorage/GetSwiftBaseURL/{}", _Params.m_CloudContext)})
			> Continuation / [=](bool _bHasPermission)
			{
				if (!_bHasPermission)
					return Continuation.f_SetException(Auditor.f_AccessDenied("(Get Swift base URL)"));

				auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
				if (!pCloudContext)
					return Continuation.f_SetException(Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext)));

				pThis->fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [Continuation, _Params](COpenStackServiceInfo &&_ServiceInfo)
					{
						if (!_ServiceInfo.m_URLs.f_Exists("swift"))
							return Continuation.f_SetException(DMibImpExceptionInstance(CExceptionCloudAPI, "Swift service not available"));

						CCloudAPIManager::CGetSwiftBaseURL::CResult Result;
						Result.m_BaseURL = _ServiceInfo.m_URLs["swift"];
						Continuation.f_SetResult(Result);
					}
				;
			}
		;

		return Continuation;
	}
}

