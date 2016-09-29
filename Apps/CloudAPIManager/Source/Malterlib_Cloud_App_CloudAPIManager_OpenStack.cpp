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
	TCContinuation<CCloudAPIManagerDaemonActor::CServer::COpenStackServiceInfo> CCloudAPIManagerDaemonActor::CServer::fp_GetOpenStackServiceInfo(CCloudContext &_CloudContext)
	{
		if (!_CloudContext.m_pGetToken || (_CloudContext.m_bLastWasError && _CloudContext.m_LastErrorClock.f_GetTime() > 5.0))
		{
			_CloudContext.m_bLastWasError = false;
			_CloudContext.m_pGetToken = fg_Construct
				(
					self
					, [this, KeystoneInfo = _CloudContext.m_KeystoneInfo]() -> TCContinuation<COpenStackServiceInfo> 
					{
						TCContinuation<COpenStackServiceInfo> Continuation;
						
						fg_Dispatch
							(
								fp_GetCURLQueryActor()
								, [Continuation, KeystoneInfo]() -> COpenStackServiceInfo
								{
									DMibError("Not implemented");
								}
							)
							> Continuation
						;
						
						return Continuation;
					}
				)
			;
		}
		TCContinuation<COpenStackServiceInfo> Continuation;
		
		if (_CloudContext.m_bLastWasError)
			return (*_CloudContext.m_pGetToken)();
		
		(*_CloudContext.m_pGetToken)() > [this, Continuation, Name = _CloudContext.f_GetName()](TCAsyncResult<COpenStackServiceInfo> &&_ServiceInfo)
			{
				auto *pCloudContext = mp_CloudContexts.f_FindEqual(Name);
				if (!_ServiceInfo)
				{
					if (pCloudContext)
					{
						pCloudContext->m_LastErrorClock.f_Start();
						pCloudContext->m_bLastWasError = true;
					}
					Continuation.f_SetException(_ServiceInfo);
					return;
				}
				else if (pCloudContext)
					pCloudContext->m_bLastWasError = false;
					
				Continuation.f_SetResult(fg_Move(*_ServiceInfo));
			}
		;
		
		return Continuation;
	}
}
