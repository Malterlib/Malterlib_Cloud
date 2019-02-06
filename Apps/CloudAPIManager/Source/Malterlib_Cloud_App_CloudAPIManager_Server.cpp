// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	CCloudAPIManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
		, mp_pCanDestroyTracker(fg_Construct())
	{
#ifdef DPlatformFamily_OSX
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");
		if (Path.f_Find("/opt/local/bin") < 0)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", "/opt/local/bin:" + Path);
#endif
		fp_Init();
	}
	
	CCloudAPIManagerDaemonActor::CServer::~CServer()
	{
	}
	
	void CCloudAPIManagerDaemonActor::CServer::fp_Init()
	{
		fg_ThisActor(this)(&CCloudAPIManagerDaemonActor::CServer::fp_SetupCloudContexs)
			> [this](TCAsyncResult<void> &&_ResultCloudContexts)
			{
				if (!_ResultCloudContexts)
				{
					DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Error, "Failed to find cloud contexts, aborting startup: {}", _ResultCloudContexts.f_GetExceptionStr());
					return;
				}
				self(&CCloudAPIManagerDaemonActor::CServer::fp_SetupPermissions) 
					> [this](TCAsyncResult<void> &&_ResultPermissions)
					{
						if (!_ResultPermissions)
						{
							DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Error, "Failed to setup permissions, aborting startup: {}", _ResultPermissions.f_GetExceptionStr());
							return;
						}
						self(&CCloudAPIManagerDaemonActor::CServer::fp_SetupDDPBridge)  
							> [this](TCAsyncResult<void> &&_ResultSetupDDPBridge)
							{
								if (!_ResultSetupDDPBridge)
								{
									DLogWithCategory(Malterlib/Cloud/CloudAPIManager, Error, "Failed to setup DDP bridge, aborting startup: {}", _ResultSetupDDPBridge.f_GetExceptionStr());
									return;
								}
								fp_Publish();
							}
						;
					}
				;
			}
		;
	}
	
	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupPermissions()
	{
		TCPromise<void> Promise;
		
		TCSet<CStr> Permissions;
		
		Permissions["ObjectStorage/GetSwiftBaseURLAll"];
		Permissions["ObjectStorage/EnsureContainerAll"];
		Permissions["ObjectStorage/DeleteObjectAll"];
		Permissions["ObjectStorage/SignTempURLAll"];
		
		for (auto &CloudContext : mp_CloudContexts)
		{
			Permissions[fg_Format("ObjectStorage/GetSwiftBaseURL/{}", CloudContext.f_GetName())];
			Permissions[fg_Format("ObjectStorage/EnsureContainer/{}", CloudContext.f_GetName())];
			Permissions[fg_Format("ObjectStorage/DeleteObject/{}", CloudContext.f_GetName())];
			Permissions[fg_Format("ObjectStorage/SignTempURL/{}", CloudContext.f_GetName())];
		}
		
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions) > fg_DiscardResult();
		
		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("ObjectStorage/*");
	
		mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this)) 
			> Promise / [this, Promise](CTrustedPermissionSubscription &&_Subscription)
			{
				mp_Permissions = fg_Move(_Subscription);
				Promise.f_SetResult();
			}
		;
		
		return Promise.f_MoveFuture();
	}
	
	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupCloudContexs()
	{
		if (auto const *pCloudContexts = mp_AppState.m_ConfigDatabase.m_Data.f_GetMember("CloudContexts", EJSONType_Object))
		{
			for (auto &CloudContextJSON : pCloudContexts->f_Object())
			{
				auto &Values = CloudContextJSON.f_Value();
				auto &CloudContext = mp_CloudContexts[CloudContextJSON.f_Name()];
				CStr Type = Values["Type"].f_String();
				if (Type == "OpenStack")
				{
					auto &KeystoneInfo = Values["KeystoneInfo"];
					CloudContext.m_KeystoneInfo.m_Username = KeystoneInfo["Username"].f_String();
					CloudContext.m_KeystoneInfo.m_Password = KeystoneInfo["Password"].f_String();
					CloudContext.m_KeystoneInfo.m_IdentityURL = KeystoneInfo["IdentityURL"].f_String();
					CloudContext.m_KeystoneInfo.m_DomainId = KeystoneInfo["DomainId"].f_String();
					CloudContext.m_KeystoneInfo.m_RegionName = KeystoneInfo["RegionName"].f_String();
					CloudContext.m_KeystoneInfo.m_TenantId = KeystoneInfo["TenantId"].f_String();
					
					auto const *pSwiftStoragePolicy = Values.f_GetMember("SwiftStoragePolicy");
					if (pSwiftStoragePolicy)
						CloudContext.m_SwiftStoragePolicy = pSwiftStoragePolicy->f_AsString("europe");
					else
						CloudContext.m_SwiftStoragePolicy = "europe";
				}
				else
					return DMibErrorInstance(fg_Format("Unknown cloud type: {}", Type));
			}
		}
		
		return fg_Explicit();
	}
	
	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		if (mp_CURLQueryActor)
			mp_CURLQueryActor->f_Destroy() > pCanDestroy->f_Track();
		return pCanDestroy->f_Future();
	}
	
	TCActor<CSeparateThreadActor> const &CCloudAPIManagerDaemonActor::CServer::fp_GetCURLQueryActor()
	{
		if (mp_CURLQueryActor)
			return mp_CURLQueryActor;
		
		mp_CURLQueryActor = fg_ConstructActor<CSeparateThreadActor>(fg_Construct("Cloud API manager CURL query file actor"));
		return mp_CURLQueryActor;
	}

	TCVector<CStr> CCloudAPIManagerDaemonActor::CServer::fsp_AuditMessages(CStr const &_Error, CExceptionPointer _pException)
	{
		TCVector<CStr> Messages;
		try
		{
			if (_pException)
				std::rethrow_exception(_pException);
			Messages.f_Insert(_Error);
		}
		catch (CExceptionCloudAPI const &_Exception)
		{
			Messages.f_Insert(fg_Format("{}: {}", _Error, _Exception.f_GetErrorStr()));
		}
		catch (CException const &_Exception)
		{
			Messages.f_Insert(_Error);
			Messages.f_Insert(fg_Format("Error: ", _Exception.f_GetErrorStr()));
		}
	
		return Messages;
	}
}
