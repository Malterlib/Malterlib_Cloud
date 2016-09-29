// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <curl/curl.h>

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCContinuation<CCloudAPIManager::CEnsureContainer::CResult> CCloudAPIManagerDaemonActor::CServer::fp_Protocol_EnsureContainer
		(
			CCallingHostInfo const &_CallingHostInfo
			, CCloudAPIManager::CEnsureContainer &&_Params
		)
	{
		TCContinuation<CCloudAPIManager::CEnsureContainer::CResult> Continuation;
		
		if (!CCloudAPIManager::fs_IsValidCloudContext(_Params.m_CloudContext))
		{
			CStr Error = "Cloud context format not valid";
			fsp_LogActivityError(_CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		
		auto *pCloudContext = mp_CloudContexts.f_FindEqual(_Params.m_CloudContext);
		if (!pCloudContext)
		{
			CStr Error = fg_Format("No such cloud context: {}", _Params.m_CloudContext);
			fsp_LogActivityError(_CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		
		fp_GetOpenStackServiceInfo(*pCloudContext) > Continuation / [this, Continuation](COpenStackServiceInfo &&_ServiceInfo)
			{
				fg_Dispatch
					(
						fp_GetCURLQueryActor()
						, [ServiceInfo = fg_Move(_ServiceInfo)]() -> CStr
						{
							return "What";
						}
					)
					> Continuation / [Continuation](CStr &&_Value)
					{
						CCloudAPIManager::CEnsureContainer::CResult Result;
						Continuation.f_SetResult(Result);
					}
				;
			}
		;
		
		return Continuation;
	}
}

