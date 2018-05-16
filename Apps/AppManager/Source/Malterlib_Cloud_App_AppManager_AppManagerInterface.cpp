// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<void> CAppManagerActor::fp_PublishAppManagerInterface()
	{
		return mp_AppManagerInterface.f_Publish<CAppManagerInterface>(mp_State.m_DistributionManager, this, CAppManagerInterface::mc_pDefaultNamespace);
	}
	
	TCContinuation<void> CAppManagerActor::fp_RegisterPermissions()
	{
		TCSet<CStr> Permissions;
		Permissions["AppManager/VersionAppAll"];
		
		Permissions["AppManager/CommandAll"];
		Permissions["AppManager/Command/VersionEnum"];
		Permissions["AppManager/Command/ApplicationEnum"];
		Permissions["AppManager/Command/ApplicationAdd"];
		Permissions["AppManager/Command/ApplicationRemove"];
		Permissions["AppManager/Command/ApplicationUpdate"];
		Permissions["AppManager/Command/ApplicationChangeSettings"];
		Permissions["AppManager/Command/ApplicationStart"];
		Permissions["AppManager/Command/ApplicationStop"];
		Permissions["AppManager/Command/ApplicationRestart"];
		Permissions["AppManager/Command/ApplicationSubscribeUpdates"];

		Permissions["AppManager/AppAll"];
		
		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			Permissions[fg_Format("AppManager/App/{}", Application.m_Name)];
		}

		return mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));
	}
	
	TCContinuation<void> CAppManagerActor::fp_RegisterApplicationPermissions(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/App/{}", _pApplication->m_Name));
		return mp_State.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, fg_Move(Permissions));
	}
	
	TCContinuation<void> CAppManagerActor::fp_UnregisterApplicationPermissions(TCSharedPointer<CApplication> const &_pApplication)
	{
		auto Permissions = fg_CreateSet<CStr>(fg_Format("AppManager/App/{}", _pApplication->m_Name));
		return mp_State.m_TrustManager(&CDistributedActorTrustManager::f_UnregisterPermissions, fg_Move(Permissions));
	}
	
	TCContinuation<void> CAppManagerActor::fp_SubscribePermissions()
	{
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("AppManager/*");
		
		TCContinuation<void> Continuation;
	
		mp_State.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}

	TCContinuation<void> CAppManagerActor::fp_SetupAppManagerInterfacePermissions()
	{
		TCContinuation<void> Continuation;
		NContainer::TCMap<NStr::CStr, CPermissionRequirements> CommandLinePermissions = {{"AppManager/VersionAppAll"}, {"AppManager/AppAll"}, {"AppManager/CommandAll"}};
		mp_State.m_TrustManager
			(
				&CDistributedActorTrustManager::f_AddPermissions
			 	, CPermissionIdentifiers{mp_State.m_CommandLineHostID, ""}
				, CommandLinePermissions
				, EDistributedActorTrustManagerOrderingFlag_None
			)
			+ fp_RegisterPermissions()
			+ fp_SubscribePermissions()
			> Continuation.f_ReceiveAny()
		;
		return Continuation;
	}
}
