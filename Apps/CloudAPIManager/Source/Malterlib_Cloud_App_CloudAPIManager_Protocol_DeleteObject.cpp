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
#include "Malterlib_Cloud_App_CloudAPIManager_CurlWrapper.h"

namespace NMib::NCloud::NCloudAPIManager
{
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_DeleteObject(CDeleteObject _Params) -> TCFuture<CDeleteObject::CResult>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			co_return Auditor.f_Exception("Cloud context format not valid");
		
		if (!CCloudAPIManager::fs_IsValidContainerName(_Params.m_ContainerName))
			co_return Auditor.f_Exception("Container name format not valid");
		
		if (!CCloudAPIManager::fs_IsValidObjectId(_Params.m_ObjectId))
			co_return Auditor.f_Exception("Object id format not valid");

		TCVector<CStr> Permissions = {"ObjectStorage/DeleteObjectAll", "ObjectStorage/DeleteObject/{}"_f << _Params.m_CloudContext};
		
		bool bHasPermission = co_await
			(
				pThis->mp_Permissions.f_HasPermission("Delete object from cloud API manager", Permissions)
				% "Permission denied deleting object"
				% Auditor
			)
		;

		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Delete object)", Permissions);

		auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			co_return Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext));

		auto ServiceInfo = co_await (pThis->fp_GetOpenStackServiceInfo(*pCloudContext) % Auditor);

		auto Value = co_await
			(
				(
					g_Dispatch(pThis->fp_GetCURLQueryActor()) / [ServiceInfo = fg_Move(ServiceInfo), _Params]() -> CStr
					{
						NException::CDisableExceptionTraceScope DisableTracing;

						if (!ServiceInfo.m_URLs.f_Exists("swift"))
							DErrorCloudAPI("Swift service not available");

						CStr URL(fg_Format("{}/{}/{}", ServiceInfo.m_URLs["swift"], _Params.m_ContainerName, _Params.m_ObjectId));

						TCMap<CStr, CStr> Headers;
						Headers["X-Auth-Token"] = ServiceInfo.m_Token;

						CCurlResult Result = fg_Curl(ECurlMethod_DELETE, URL, Headers, CStr());
						if (Result.m_StatusCode != 204 && Result.m_StatusCode != 404)
							DErrorCloudAPI(fg_Format("Unexpected result {} {}", Result.m_StatusCode, Result.m_StatusMessage));

						return URL;
					}
				)
				.f_Wrap()
			)
		;

		if (!Value)
		{
			CStr Error = fg_Format("Failed to delete object {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext);
			co_return Auditor.f_Exception(fsp_AuditMessages(Error, Value.f_GetException()));
		}

		CCloudAPIManager::CDeleteObject::CResult Result;
		Auditor.f_Info(fg_Format("Deleted object {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext));

		co_return fg_Move(Result);
	}
}

