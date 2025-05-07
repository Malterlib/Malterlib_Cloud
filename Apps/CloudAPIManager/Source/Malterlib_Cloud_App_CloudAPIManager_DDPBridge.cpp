// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Encoding/JsonShortcuts>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupDDPBridge()
	{
		mp_DDPBridge = fg_ConstructActor<CDistributedTrustDDPBridge>(mp_AppState.m_TrustManager);
		
		mp_DDPBridgeSubscription = co_await mp_DDPBridge(&CDistributedTrustDDPBridge::f_RegisterMethods, fp_GetDDPMethods());

		co_await mp_DDPBridge(&CDistributedTrustDDPBridge::f_Startup);

		co_return {};
	}
	
	TCVector<CDistributedTrustDDPBridge::CMethod> CCloudAPIManagerDaemonActor::CServer::fp_GetDDPMethods()
	{
		return NContainer::fg_CreateVector<CDistributedTrustDDPBridge::CMethod>
			(
				CDistributedTrustDDPBridge::CMethod
				{
					"getSwiftBaseURL"
					, g_ActorFunctor / [this](NContainer::TCVector<NEncoding::CEJsonSorted> _Params) -> TCFuture<NEncoding::CEJsonSorted>
					{
						CCloudAPIManager::CGetSwiftBaseURL Params;
						{
							auto CaptureScope = co_await g_CaptureExceptions;

							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String();
						}

						co_return (co_await mp_ProtocolInterface.m_pActor->f_GetSwiftBaseURL(fg_Move(Params))).m_BaseURL;
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIEnsureContainer"
					, g_ActorFunctor / [this](NContainer::TCVector<NEncoding::CEJsonSorted> _Params) -> TCFuture<NEncoding::CEJsonSorted>
					{
						if (_Params.f_GetLen() != 1)
							co_return "Method takes 1 parameter";
						CCloudAPIManager::CEnsureContainer Params;
						{
							auto CaptureScope = co_await g_CaptureExceptions;

							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String(); 
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_TempURLKey = InputParams["tempURLKey"].f_String();
						}

						co_await mp_ProtocolInterface.m_pActor->f_EnsureContainer(fg_Move(Params));

						co_return {};
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPISignTempURL"
					, g_ActorFunctor / [this](NContainer::TCVector<NEncoding::CEJsonSorted> _Params) -> TCFuture<NEncoding::CEJsonSorted>
					{
						if (_Params.f_GetLen() != 1)
							co_return "Method takes 1 parameter";
						CCloudAPIManager::CSignTempURL Params;
						{
							auto CaptureScope = co_await g_CaptureExceptions;

							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String();
							Params.m_Method = InputParams["method"].f_String();
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_ObjectId = InputParams["objectId"].f_String();
							Params.m_TempURLKey = InputParams["tempURLKey"].f_String();
						}

						co_return (co_await mp_ProtocolInterface.m_pActor->f_SignTempURL(fg_Move(Params))).m_SignedURL;
					}
				}
				, CDistributedTrustDDPBridge::CMethod
				{
					"cloudAPIDeleteObject"
					, g_ActorFunctor / [this](NContainer::TCVector<NEncoding::CEJsonSorted> _Params) -> TCFuture<NEncoding::CEJsonSorted>
					{
						if (_Params.f_GetLen() != 1)
							co_return "Method takes 1 parameter";
						CCloudAPIManager::CDeleteObject Params;
						{
							auto CaptureScope = co_await g_CaptureExceptions;

							NException::CDisableExceptionTraceScope DisableTracing;
							auto &InputParams = _Params[0];
							Params.m_CloudContext = InputParams["cloudContext"].f_String(); 
							Params.m_ContainerName = InputParams["containerName"].f_String();
							Params.m_ObjectId = InputParams["objectId"].f_String();
						}
						
						co_await mp_ProtocolInterface.m_pActor->f_DeleteObject(fg_Move(Params));

						co_return {};
					}
				}
			)
		;
	}
}
