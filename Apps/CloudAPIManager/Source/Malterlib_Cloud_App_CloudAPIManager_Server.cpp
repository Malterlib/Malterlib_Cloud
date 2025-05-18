// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Concurrency/LogError>

#include "Malterlib_Cloud_App_CloudAPIManager.h"
#include "Malterlib_Cloud_App_CloudAPIManager_Server.h"

namespace NMib::NCloud::NCloudAPIManager
{
	CCloudAPIManagerDaemonActor::CServer::CServer(CDistributedAppState &_AppState)
		: mp_AppState(_AppState)
		, mp_pCanDestroyTracker(fg_Construct())
	{
#ifdef DPlatformFamily_macOS
		CStr Path = fg_GetSys()->f_GetEnvironmentVariable("PATH");

		CStr OriginalPath = Path;

		if (Path.f_Find("/usr/local/bin") < 0)
			Path = "/usr/local/bin:" + Path;
		if (Path.f_Find("/opt/homebrew/bin") < 0)
			Path = "/opt/homebrew/bin:" + Path;

		if (Path != OriginalPath)
			fg_GetSys()->f_SetEnvironmentVariable("PATH", Path);
#endif
		fp_Init() > fg_LogError("CloudAPIManager", "Failed to initialize server");
	}

	CCloudAPIManagerDaemonActor::CServer::~CServer()
	{
	}

	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_Init()
	{
		co_await (fp_SetupCloudContexs() % "Failed to find cloud contexts, aborting startup");
		co_await (fp_SetupPermissions() % "Failed to setup permissions, aborting startup");
		co_await (fp_SetupDDPBridge() % "Failed to setup DDP bridge, aborting startup");

		fp_Publish();

		co_return {};
	}

	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupPermissions()
	{
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

		co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, Permissions).f_Wrap() > fg_LogError("CloudAPIManager", "Failed to register permissions");

		TCVector<CStr> SubscribePermissions;
		SubscribePermissions.f_Insert("ObjectStorage/*");

		mp_Permissions = co_await mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_SubscribeToPermissions, SubscribePermissions, fg_ThisActor(this));

		co_return {};
	}

	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_SetupCloudContexs()
	{
		if (auto const *pCloudContexts = mp_AppState.m_ConfigDatabase.m_Data.f_GetMember("CloudContexts", EJsonType_Object))
		{
			for (auto &CloudContextJson : pCloudContexts->f_Object())
			{
				auto &Values = CloudContextJson.f_Value();
				auto &CloudContext = mp_CloudContexts[CloudContextJson.f_Name()];
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
					co_return DMibErrorInstance(fg_Format("Unknown cloud type: {}", Type));
			}
		}

		co_return {};
	}

	TCFuture<void> CCloudAPIManagerDaemonActor::CServer::fp_Destroy()
	{
		auto pCanDestroy = fg_Move(mp_pCanDestroyTracker);
		mp_ProtocolInterface.f_Destroy() > pCanDestroy->f_Track();
		if (mp_CURLQueryActor)
			fg_Move(mp_CURLQueryActor).f_Destroy() > pCanDestroy->f_Track();

		auto CanDestroyFuture = pCanDestroy->f_Future();
		pCanDestroy.f_Clear();
		co_await fg_Move(CanDestroyFuture);

		co_return {};
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

		bool bHandled = false;
		if (_pException)
		{
			bHandled = NException::fg_VisitException<CExceptionCloudAPI, CException>
				(
					_pException
					, [&]<typename tf_CException>(tf_CException const &_Exception)
					{
						if constexpr (NTraits::cIsSame<tf_CException, CExceptionCloudAPI>)
						{
							Messages.f_Insert(fg_Format("{}: {}", _Error, _Exception.f_GetErrorStr()));
						}
						else
						{
							Messages.f_Insert(_Error);
							Messages.f_Insert(fg_Format("Error: ", _Exception.f_GetErrorStr()));
						}
					}
				)
			;
		}

		if (!bHandled)
			Messages.f_Insert(_Error);

		return Messages;
	}
}
