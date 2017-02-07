// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>> CAppManagerActor::CDistributedAppInterfaceServerImplementation::f_RegisterDistributedApp
		(
			NConcurrency::TCDistributedActorInterfaceWithID<CDistributedAppInterfaceClient> &&_ClientInterface
			, NConcurrency::TCDistributedActorInterfaceWithID<CDistributedActorTrustManagerInterface> &&_TrustInterface
			, CRegisterInfo const &_RegisterInfo
		)
	{
		auto pThis = m_pThis;
		
		CCallingHostInfo CallingHostInfo = NConcurrency::fg_GetCallingHostInfo();
		auto &HostID = CallingHostInfo.f_GetRealHostID();
		
		for (auto &pApplication : pThis->mp_Applications)
		{
			auto &Application = *pApplication;
			if (Application.m_AssociatedHostID == HostID)
			{
				Application.m_AppInterface = fg_Move(_ClientInterface);
				if (Application.m_RegisterInfo != _RegisterInfo)
				{
					Application.m_RegisterInfo = _RegisterInfo;
					pThis->fp_RemoteAppInfoChanged(pApplication);
					pThis->fp_UpdateApplicationJSON(pApplication) > fg_DiscardResult();
				}
				
				DMibLogWithCategory
					(
						Malterlib/Cloud/AppManager
						, Info
						, "Application '{}' registered from host '{}' and uses update type '{}'"
						, Application.m_Name
						, CallingHostInfo.f_GetHostInfo().f_GetDesc()
						, _RegisterInfo.m_UpdateType
					)
				;
				
				if (Application.m_fOnRegisterDistributedApp)
				{
					Application.m_fOnRegisterDistributedApp();
					Application.m_fOnRegisterDistributedApp.f_Clear();
				}
				
				return fg_Explicit
					(
						g_ActorSubscription > [pApplication, AssignSequence = ++Application.m_AppInterfaceAssignSequence, HostInfo = CallingHostInfo.f_GetHostInfo()]
						{
							if (pApplication->m_bDeleted || AssignSequence != pApplication->m_AppInterfaceAssignSequence)
								return;
							
							auto &Application = *pApplication;
							Application.m_AppInterface.f_Clear();
							
							DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Application registration lost: {}", HostInfo.f_GetDesc());
						}
					)
				;
				break;
			}
		}
		
		DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Unassociated application registered: {}", CallingHostInfo.f_GetHostInfo().f_GetDesc());
		return DErrorInstance("Application not associated with your host");
	}
	
	TCContinuation<void> CAppManagerActor::fp_PublishAppInterface()
	{
		return mp_AppInterfaceServer.f_Publish<CDistributedAppInterfaceServer>(mp_State.m_DistributionManager, this, CDistributedAppInterfaceServer::mc_pDefaultNamespace);
	}
}
