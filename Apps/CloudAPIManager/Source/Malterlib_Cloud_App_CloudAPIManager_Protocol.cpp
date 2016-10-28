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
	CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::CCloudAPIManagerImplementation(TCActor<CCloudAPIManagerDaemonActor::CServer> &&_Server)
		: mp_Server(_Server)
	{
		DMibPublishActorFunction(CCloudAPIManager::f_EnsureContainer);
	}

	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_GetSwiftBaseURL(CGetSwiftBaseURL &&_Params)
		-> TCContinuation<CGetSwiftBaseURL::CResult>
	{
		return mp_Server(&CCloudAPIManagerDaemonActor::CServer::fp_Protocol_GetSwiftBaseURL, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_EnsureContainer(CEnsureContainer &&_Params)
		-> TCContinuation<CEnsureContainer::CResult> 
	{
		return mp_Server(&CCloudAPIManagerDaemonActor::CServer::fp_Protocol_EnsureContainer, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_SignTempURL(CSignTempURL &&_Params)
		-> TCContinuation<CSignTempURL::CResult>
	{
		return mp_Server(&CCloudAPIManagerDaemonActor::CServer::fp_Protocol_SignTempURL, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CCloudAPIManagerDaemonActor::CServer::CCloudAPIManagerImplementation::f_DeleteObject(CDeleteObject &&_Params)
		-> TCContinuation<CDeleteObject::CResult>
	{
		return mp_Server(&CCloudAPIManagerDaemonActor::CServer::fp_Protocol_DeleteObject, fg_GetCallingHostInfo(), fg_Move(_Params));
	}

	void CCloudAPIManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolImplementation = fg_ConstructDistributedActor<CCloudAPIManagerImplementation>(fg_ThisActor(this));
		
		auto &DistributionManager = NConcurrency::fg_GetDistributionManager();
		DistributionManager
			(
				&CActorDistributionManager::f_PublishActor
				, mp_ProtocolImplementation
				, "com.malterlib/Cloud/CloudAPIManager"
				, NConcurrency::CDistributedActorInheritanceHeirarchyPublish::fs_GetHierarchy<CCloudAPIManager>()
			)
			> [this] (TCAsyncResult<CDistributedActorPublication> &&_Publication)
			{
				mp_ProtocolPublication = fg_Move(*_Publication);
			}
		;
	}

	NException::CException CCloudAPIManagerDaemonActor::CServer::fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description, CStr const &_UserDescription)
	{
		if (!_UserDescription.f_IsEmpty())
		{
			fsp_LogActivityWarning(_CallingHostInfo, fg_Format("Denied access to: {} '{}'", _Description, _UserDescription));
			return DMibErrorInstance(_UserDescription);
		}
		else
		{
			fsp_LogActivityWarning(_CallingHostInfo, fg_Format("Denied access to: {}", _Description));
			return DMibErrorInstance("Access denied");
		}
	}
}
