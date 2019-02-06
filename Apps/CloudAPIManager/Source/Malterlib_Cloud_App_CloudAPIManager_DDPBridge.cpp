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
	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupDDPBridge()
	{
		mp_DDPBridge = fg_ConstructActor<CDistributedTrustDDPBridge>(mp_AppState.m_TrustManager);
		
		TCPromise<void> Promise;
		mp_DDPBridge(&CDistributedTrustDDPBridge::f_RegisterMethods, fg_ThisActor(this).f_Weak(), fp_GetDDPMethods()) > Promise / [this, Promise](CActorSubscription &&_ActorSub)
			{
				mp_DDPBridgeSubscription = fg_Move(_ActorSub);
				mp_DDPBridge(&CDistributedTrustDDPBridge::f_Startup) > Promise;
			}
		;		

		return Promise.f_MoveFuture();
	}
	
	TCVector<CDistributedTrustDDPBridge::CMethod> CCloudAPIManagerDaemonActor::CServer::fp_GetDDPMethods()
	{
		return NContainer::fg_CreateVector<CDistributedTrustDDPBridge::CMethod>
			(
			 	CDistributedTrustDDPBridge::CMethod
				{
					"getSwiftBaseURL"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCFuture<NEncoding::CEJSON> 
					{
						TCPromise<NEncoding::CEJSON> Promise;
						CCloudAPIManager::CGetSwiftBaseURL Params;
						try
						{
							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String();
						}
						catch (NException::CException const &_Exception)
						{
							return _Exception;
						}
						
						mp_ProtocolInterface.m_pActor->f_GetSwiftBaseURL(fg_Move(Params)) > Promise / [Promise](CCloudAPIManager::CGetSwiftBaseURL::CResult &&_Result)
							{
								NEncoding::CEJSON Result = _Result.m_BaseURL;
								Promise.f_SetResult(Result);
							}
						;
						return Promise.f_MoveFuture();
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIEnsureContainer"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCFuture<NEncoding::CEJSON>
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCPromise<NEncoding::CEJSON> Promise;
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
						mp_ProtocolInterface.m_pActor->f_EnsureContainer(fg_Move(Params)) > Promise / [Promise](CCloudAPIManager::CEnsureContainer::CResult &&_Result)
							{
								NEncoding::CEJSON Result;
								Promise.f_SetResult(Result);
							}
						;
						return Promise.f_MoveFuture();
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPISignTempURL"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCFuture<NEncoding::CEJSON> 
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCPromise<NEncoding::CEJSON> Promise;
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
						mp_ProtocolInterface.m_pActor->f_SignTempURL(fg_Move(Params)) > Promise / [Promise](CCloudAPIManager::CSignTempURL::CResult &&_Result)
							{
								NEncoding::CEJSON Result = _Result.m_SignedURL;
								Promise.f_SetResult(Result);
							}
						;
						return Promise.f_MoveFuture();
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIDeleteObject"
					, [this](NContainer::TCVector<NEncoding::CEJSON> const &_Params) -> TCFuture<NEncoding::CEJSON>
					{
						if (_Params.f_GetLen() != 1)
							return fg_Explicit("Method takes 1 parameter");
						TCPromise<NEncoding::CEJSON> Promise;
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
						mp_ProtocolInterface.m_pActor->f_DeleteObject(fg_Move(Params)) > Promise / [Promise](CCloudAPIManager::CDeleteObject::CResult &&_Result)
							{
								NEncoding::CEJSON Result;
								Promise.f_SetResult(Result);
							}
						;
						return Promise.f_MoveFuture();
					}
				}
			)
		;
	}
}
