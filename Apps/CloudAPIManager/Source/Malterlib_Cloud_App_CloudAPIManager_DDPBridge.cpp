// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JSONShortcuts>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCContinuation<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupDDPBridge()
	{
		mp_DDPBridge = fg_ConstructActor<CDistributedTrustDDPBridge>(mp_AppState.m_TrustManager);
		
		TCContinuation<void> Continuation;
		mp_DDPBridge(&CDistributedTrustDDPBridge::f_RegisterMethods, fg_ThisActor(this).f_Weak(), fp_GetDDPMethods()) > Continuation / [this, Continuation](CActorSubscription &&_ActorSub)
			{
				mp_DDPBridgeSubscription = fg_Move(_ActorSub);
				mp_DDPBridge(&CDistributedTrustDDPBridge::f_Startup) > Continuation;
			}
		;		

		return Continuation;
	}
	
	TCVector<CDistributedTrustDDPBridge::CMethod> CCloudAPIManagerDaemonActor::CServer::fp_GetDDPMethods()
	{
		return NContainer::fg_CreateVector<CDistributedTrustDDPBridge::CMethod>
			(
				CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIEnsureContainer"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCContinuation<NEncoding::CEJSON>
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCContinuation<NEncoding::CEJSON> Continuation;
						CCloudAPIManager::CEnsureContainer Params;
						try
						{
							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String(); 
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_TempURLKey = InputParams["tempURLKey"].f_String();
						}
						catch (NException::CException const &_Exception)
						{
							return _Exception;
						}
						fp_Protocol_EnsureContainer(fg_GetCallingHostInfo(), fg_Move(Params)) > Continuation / [Continuation](CCloudAPIManager::CEnsureContainer::CResult &&_Result)
							{
								NEncoding::CEJSON Result;
								Continuation.f_SetResult(Result);
							}
						;
						return Continuation;
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPISignTempURL"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCContinuation<NEncoding::CEJSON> 
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCContinuation<NEncoding::CEJSON> Continuation;
						CCloudAPIManager::CSignTempURL Params;
						try
						{
							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String();
							Params.m_Method = InputParams["method"].f_String();
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_ObjectId = InputParams["objectId"].f_String();
							Params.m_TempURLKey = InputParams["tempURLKey"].f_String();
						}
						catch (NException::CException const &_Exception)
						{
							return _Exception;
						}
						fp_Protocol_SignTempURL(fg_GetCallingHostInfo(), fg_Move(Params)) > Continuation / [Continuation](CCloudAPIManager::CSignTempURL::CResult &&_Result)
							{
								NEncoding::CEJSON Result = _Result.m_SignedURL;
								Continuation.f_SetResult(Result);
							}
						;
						return Continuation;
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIDeleteObject"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCContinuation<NEncoding::CEJSON>
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCContinuation<NEncoding::CEJSON> Continuation;
						CCloudAPIManager::CDeleteObject Params;
						try
						{
							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String(); 
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_ObjectId = InputParams["objectId"].f_String();
						}
						catch (NException::CException const &_Exception)
						{
							return _Exception;
						}
						fp_Protocol_DeleteObject(fg_GetCallingHostInfo(), fg_Move(Params)) > Continuation / [Continuation](CCloudAPIManager::CDeleteObject::CResult &&_Result)
							{
								NEncoding::CEJSON Result;
								Continuation.f_SetResult(Result);
							}
						;
						return Continuation;
					}
				}
			)
		;
	}
}
