// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>

#include "Malterlib_Cloud_App_SecretsManager.h"
#include "Malterlib_Cloud_App_SecretsManager_Server.h"

namespace NMib::NCloud::NSecretsManager
{
	CSecretsManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
		, mp_pCanDestroyTracker(fg_Construct())
	{
		fp_Init();
	}
	
	CSecretsManagerDaemonActor::CServer::~CServer()
	{
	}
	
	void CSecretsManagerDaemonActor::CServer::fp_Init()
	{
		fp_SetupPermissions() > [this](TCAsyncResult<void> &&_ResultPermissions)
			{
				if (!_ResultPermissions)
				{
					DLogWithCategory(Malterlib/Cloud/SecretsManager, Error, "Failed to setup permissions, aborting startup: {}", _ResultPermissions.f_GetExceptionStr());
					return;
				}
				fp_Publish();
			}
		;
	}
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCContinuation<void> Continuation;
		
		TCSet<CStr> Permissions;
		
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
		
		TCVector<CStr> SubscribePermissions;

		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this))
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	
	TCContinuation<void> CSecretsManagerDaemonActor::CServer::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		return pCanDestroy->m_Continuation;
	}
}
