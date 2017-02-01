// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"
#include "Malterlib_Cloud_App_CloudAPIManager_CurlWrapper.h"

namespace NMib::NCloud::NCloudAPIManager
{
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_EnsureContainer(CEnsureContainer &&_Params) -> TCContinuation<CEnsureContainer::CResult> 
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		TCContinuation<CCloudAPIManager::CEnsureContainer::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
			return Auditor.f_Exception("Cloud context format not valid");
		
		if (!CCloudAPIManager::fs_IsValidContainerName(_Params.m_ContainerName))
			return Auditor.f_Exception("Container name format not valid");
		
		// Empty key locks the container
		if (!_Params.m_TempURLKey.f_IsEmpty() &&  !CCloudAPIManager::fs_IsValidTempURLKey(_Params.m_TempURLKey))
			return Auditor.f_Exception("Temp URL key format not valid");
		
		if (!pThis->mp_Permissions.f_HostHasAnyPermission(fg_GetCallingHostID(), "ObjectStorage/EnsureContainerAll", fg_Format("ObjectStorage/EnsureContainer/{}", _Params.m_CloudContext)))
			return Auditor.f_AccessDenied("(Ensure container)");
		
		auto *pCloudContext = pThis->mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
			return Auditor.f_Exception(fg_Format("No such cloud context: {}", _Params.m_CloudContext));
		
		CStr StoragePolicy = pCloudContext->m_SwiftStoragePolicy;
		
		pThis->fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [pThis, Continuation, _Params, StoragePolicy, Auditor](COpenStackServiceInfo &&_ServiceInfo)
			{
				fg_Dispatch
					(
						pThis->fp_GetCURLQueryActor()
						, [ServiceInfo = fg_Move(_ServiceInfo), _Params, StoragePolicy]() -> CStr
						{
							NException::CDisableExceptionTraceScope DisableTracing;
							
							if (!ServiceInfo.m_URLs.f_Exists("swift"))
								DErrorCloudAPI("Swift service not available");
							
							if (_Params.m_ContainerName.f_IsEmpty())
								DErrorCloudAPI("Parameter containerName is empty");
							
							CStr URL(fg_Format("{}/{}", ServiceInfo.m_URLs["swift"], _Params.m_ContainerName));
							
							TCMap<CStr, CStr> Headers;
							
							Headers["X-Auth-Token"] = ServiceInfo.m_Token;
							Headers["X-Storage-Policy"] = StoragePolicy;
							
							if (_Params.m_TempURLKey.f_IsEmpty())
							{
								Headers["X-Remove-Container-Read"] = "true";
								Headers["X-Remove-Container-Meta-Temp-URL-Key"] = "true";
								Headers["X-Remove-Container-Meta-Access-Control-Allow-Origin"] = "true";
							}
							else
							{
								Headers["X-Container-Read"] = ".r:*";
								Headers["X-Container-Meta-Temp-URL-Key"] = _Params.m_TempURLKey;
								Headers["X-Container-Meta-Access-Control-Allow-Origin"] = "*";
							}
							
							CCurlResult Result = fg_Curl(ECurlMethod_PUT, URL, Headers, CStr());
							if (Result.m_StatusCode >= 300)
								DErrorCloudAPI(fg_Format("Unexpected result {} {}", Result.m_StatusCode, Result.m_StatusMessage));
							
							return URL;
						}
					)
					> [Continuation, _Params, Auditor](TCAsyncResult<CStr> &&_Value)
					{
						if (!_Value)
						{
							CStr Error = fg_Format("Failed to ensure container {} on {}", _Params.m_ContainerName, _Params.m_CloudContext);
							Continuation.f_SetException(Auditor.f_Exception(fsp_AuditMessages(Error, _Value.f_GetException())));
							return;
						}
						
						CCloudAPIManager::CEnsureContainer::CResult Result;
						Continuation.f_SetResult(Result);
						Auditor.f_Info(fg_Format("Ensure container {} on {}", _Params.m_ContainerName, _Params.m_CloudContext));
					}
				;
			}
		;
		
		return Continuation;
	}
}

