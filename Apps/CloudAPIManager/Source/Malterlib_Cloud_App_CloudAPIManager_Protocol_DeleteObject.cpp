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
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_DeleteObject(CDeleteObject &&_Params) -> TCContinuation<CDeleteObject::CResult>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		TCContinuation<CCloudAPIManager::CDeleteObject::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return Auditor.f_Exception("Cloud context format not valid");
		
		if (!CCloudAPIManager::fs_IsValidContainerName(_Params.m_ContainerName))
			return Auditor.f_Exception("Container name format not valid");
		
		if (!CCloudAPIManager::fs_IsValidObjectId(_Params.m_ObjectId))
			return Auditor.f_Exception("Object id format not valid");
		
		if (!pThis->mp_Permissions.f_HostHasAnyPermission(fg_GetCallingHostID(), "ObjectStorage/DeleteObjectAll", fg_Format("ObjectStorage/DeleteObject/{}", _Params.m_CloudContext)))
			return Auditor.f_AccessDenied("(Delete object)");
		
		auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			return Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext));
		
		pThis->fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [pThis, Continuation, _Params, Auditor](COpenStackServiceInfo &&_ServiceInfo)
			{
				fg_Dispatch
					(
						pThis->fp_GetCURLQueryActor()
						, [ServiceInfo = fg_Move(_ServiceInfo), _Params]() -> CStr
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
					> [Continuation, _Params, Auditor](TCAsyncResult<CStr> &&_Value)
					{
						if (!_Value)
						{
							CStr Error = fg_Format("Failed to delete object {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext);
							Continuation.f_SetException(Auditor.f_Exception(fsp_AuditMessages(Error, _Value.f_GetException())));
							return;
						}

						CCloudAPIManager::CDeleteObject::CResult Result;
						Continuation.f_SetResult(Result);
						Auditor.f_Info(fg_Format("Deleted object {}/{} on {}", _Params.m_ContainerName, _Params.m_ObjectId, _Params.m_CloudContext));
					}
				;
			}
		;
		
		return Continuation;
	}
}

