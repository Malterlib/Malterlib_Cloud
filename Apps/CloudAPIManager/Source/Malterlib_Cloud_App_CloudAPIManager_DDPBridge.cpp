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
		CEJSON const *pConfig;
		if (!(pConfig = mp_AppState.m_ConfigDatabase.m_Data.f_GetMember("DDPBridge", EJSONType_Object)))
		{
			DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Warning, "DDP bridge not set up: 'DDPBridge' config missing in '{}'", mp_AppState.m_ConfigDatabase.f_GetFileName());
			return fg_Explicit();
		}
		
		auto &Config = *pConfig;
		
		CEJSON const *pListen;
		if (!(pListen = Config.f_GetMember("Listen", EJSONType_Array)))
		{
			DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Warning, "DDP bridge not set up: 'DDPBridge.Listen' missing in '{}'", mp_AppState.m_ConfigDatabase.f_GetFileName());
			return fg_Explicit();
		}
		
		auto &Listen = pListen->f_Array();
		
		if (Listen.f_IsEmpty())
		{
			DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Warning, "DDP bridge not set up: 'DDPBridge.Listen' is empty in '{}'", mp_AppState.m_ConfigDatabase.f_GetFileName());
			return fg_Explicit();
		}
		
		TCVector<CURL> ListenURLs;
		
		for (auto &ListenEntry : Listen)
		{
			if (auto *pValue = ListenEntry.f_GetMember("Address", EJSONType_String))
			{
				CURL URL;
				if (!URL.f_Decode(pValue->f_String()))
				{
					DLogWithCategory
						(
							Malterlib/Cloud/CloudAPIManager
							, Warning
							, "Invalid address '{}' in 'DDPBridge.Listen' in '{}'"
							, pValue->f_String()
							, mp_AppState.m_ConfigDatabase.f_GetFileName()
						)
					;
					continue;
				}
				ListenURLs.f_Insert(URL);
			}
			else
			{
				DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Warning, "Invalid address in 'DDPBridge.Listen' in '{}'", mp_AppState.m_ConfigDatabase.f_GetFileName());
			}
		}
		
		if (ListenURLs.f_IsEmpty())
		{
			DLogWithCategory
				(
					Malterlib/Cloud/CloudAPIManager
					, Warning
					, "DDP bridge not set up: No valid addresses in 'DDPBridge.Listen' in '{}'"
					, mp_AppState.m_ConfigDatabase.f_GetFileName()
				)
			;
			return fg_Explicit();
		}
		
		mp_DDPBridge = fg_ConstructActor<CDistributedTrustDDPBridge>(ListenURLs, mp_AppState.m_TrustManager);
		
		TCContinuation<void> Continuation;
		
		mp_DDPBridge(&CDistributedTrustDDPBridge::f_RegisterMethods, fg_ThisActor(this).f_Weak(), fp_GetDDPMethods())  > Continuation / [this, Continuation]
			{
				mp_DDPBridge(&CDistributedTrustDDPBridge::f_Startup) > Continuation;
			}
		;		

		return Continuation;
	}
	
	TCVector<CDistributedTrustDDPBridge::CMethod> CCloudAPIManagerDaemonActor::CServer::fp_GetDDPMethods()
	{
		return NContainer::fg_CreateVector<CDistributedTrustDDPBridge::CMethod>
			(
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
								NEncoding::CEJSON Result = EJSONType_Object;
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
