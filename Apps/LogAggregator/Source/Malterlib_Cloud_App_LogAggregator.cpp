// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_LogAggregator.h"

namespace NMib::NCloud::NLogAggregator
{
	CLogAggregatorServer::CLogAggregatorServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
	{
	}
	
	CLogAggregatorServer::~CLogAggregatorServer()
	{
	}
	
	TCContinuation<void> CLogAggregatorServer::f_Init()
	{
		fp_Publish();
		return fg_Explicit();
	}
	
	TCContinuation<void> CLogAggregatorServer::fp_SetupPermissions()
	{
		TCContinuation<void> Continuation;
		
		TCSet<CStr> Permissions;
		Permissions["LogAggregator/WriteSelf"];

		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
		
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("LogAggregator/*");
	
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
			> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Continuation.f_SetResult();
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CLogAggregatorServer::fp_Destroy()
	{
		return fg_Explicit();
	}
}
