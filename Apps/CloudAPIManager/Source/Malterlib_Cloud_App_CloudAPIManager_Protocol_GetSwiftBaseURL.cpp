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
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_GetSwiftBaseURL(CGetSwiftBaseURL &&_Params) -> TCFuture<CGetSwiftBaseURL::CResult>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			co_return Auditor.f_Exception("Cloud context format not valid");

		TCVector<CStr> Permissions = {"ObjectStorage/GetSwiftBaseURLAll", "ObjectStorage/GetSwiftBaseURL/{}"_f << _Params.m_CloudContext};
		
		bool bHasPermission = co_await
			(
			 	pThis->mp_Permissions.f_HasPermission("Get swift base URL", Permissions)
			 	% "Permission denied getting swift base URL"
			 	% Auditor
			)
		;
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Get Swift base URL)", Permissions);

		auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			co_return Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext));

		auto ServiceInfo = co_await pThis->fp_GetOpenStackServiceInfo(*pCloudContext);

		if (!ServiceInfo.m_URLs.f_Exists("swift"))
			co_return DMibImpExceptionInstance(CExceptionCloudAPI, "Swift service not available");

		CCloudAPIManager::CGetSwiftBaseURL::CResult Result;
		Result.m_BaseURL = ServiceInfo.m_URLs["swift"];

		co_return fg_Move(Result);
	}
}

