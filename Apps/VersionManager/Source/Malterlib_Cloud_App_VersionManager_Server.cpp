// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	CVersionManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
		, mp_pCanDestroyTracker(fg_Construct())
	{
#ifdef DPlatformFamily_OSX
		CStr Path = NSys::fg_Process_GetEnvironmentVariable(CStr("PATH"));
		if (Path.f_Find("/opt/local/bin") < 0)
			NSys::fg_Process_SetEnvironmentVariable(CStr("PATH"), "/opt/local/bin:" + Path);
#endif
	}
	
	CVersionManagerDaemonActor::CServer::~CServer()
	{
	}
	
	void CVersionManagerDaemonActor::CServer::f_Construct()
	{
		fp_Init();
	}
	
	void CVersionManagerDaemonActor::CServer::fp_Init()
	{
		fg_ThisActor(this)(&CVersionManagerDaemonActor::CServer::fp_SetupPermissions) > [this](TCAsyncResult<void> &&_Result)
			{
				if (!_Result)
				{
					DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "Failed to setup permissions, aborting startup: {}", _Result.f_GetExceptionStr());
					return;
				}
				fp_Publish();
			}
		;
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCContinuation<void> Continuation;
		
		fg_ThisActor(this)(&CVersionManagerDaemonActor::CServer::fp_EnumApplications) > Continuation / [this, Continuation](TCSet<CStr> &&_Applications)
			{
				TCSet<CStr> Permissions;
				Permissions["Application/ReadAll"];
				Permissions["Application/ListAll"];
				Permissions["Application/WriteAll"];
				
				for (auto &Application : _Applications)
				{
					Permissions[fg_Format("Application/Read/{}", Application)];
					Permissions[fg_Format("Application/Write/{}", Application)];
				}
				
				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
				
				TCVector<CStr> SubscribePermissions;
				SubscribePermissions.f_Insert("Application/*");
			
				mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
					> Continuation / [this, Continuation](CTrustedPermissionSubscription &&_Subscription)
					{
						mp_Permissions = fg_Move(_Subscription);
						Continuation.f_SetResult();
					}
				;
			}
		;
		
		return Continuation;
	}
	
	TCContinuation<void> CVersionManagerDaemonActor::CServer::f_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		mp_ProtocolPublication.f_Clear();
		if (mp_QueryFileActor)
		{
			mp_QueryFileActor->f_Destroy
				(
					[pCanDestroy](TCAsyncResult<void> &&)
					{
					}
				)
			;
		}
		return pCanDestroy->m_Continuation;
	}
	
	TCActor<CSeparateThreadActor> const &CVersionManagerDaemonActor::CServer::fp_GetQueryFileActor()
	{
		if (mp_QueryFileActor)
			return mp_QueryFileActor;
		
		mp_QueryFileActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Version manager query file actor"));
		return mp_QueryFileActor;
	}
	
	void CVersionManagerDaemonActor::CServer::fsp_LogActivityError(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error)
	{
		DLogWithCategory(Malterlib/Cloud/VersionManager, Error, "({}) {}", _CallingHostInfo.f_GetHostInfo(), _Error);
	}

	void CVersionManagerDaemonActor::CServer::fsp_LogActivityWarning(CCallingHostInfo const &_CallingHostInfo, CStr const &_Error)
	{
		DLogWithCategory(Malterlib/Cloud/VersionManager, Warning, "({}) {}", _CallingHostInfo.f_GetHostInfo(), _Error);
	}

	void CVersionManagerDaemonActor::CServer::fsp_LogActivityInfo(CCallingHostInfo const &_CallingHostInfo, CStr const &_Info)
	{
		DLogWithCategory(Malterlib/Cloud/VersionManager, Info, "({}) {}", _CallingHostInfo.f_GetHostInfo(), _Info);
	}
}
