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
	CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::CVersionManagerImplementation(TCActor<CVersionManagerDaemonActor::CServer> &&_Server)
		: mp_Server(_Server)
	{
		DMibPublishActorFunction(CVersionManager::f_ListApplications);
		DMibPublishActorFunction(CVersionManager::f_ListVersions);
		DMibPublishActorFunction(CVersionManager::f_UploadVersion);
		DMibPublishActorFunction(CVersionManager::f_DownloadVersion);
		DMibPublishActorFunction(CVersionManager::f_SubscribeToUpdates);
	}
		
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListApplications(CListApplications &&_Params)
		-> TCContinuation<CListApplications::CResult> 
	{
		return mp_Server(&CVersionManagerDaemonActor::CServer::fp_Protocol_ListApplications, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListVersions(CListVersions &&_Params)
		-> TCContinuation<CListVersions::CResult>
	{
		return mp_Server(&CVersionManagerDaemonActor::CServer::fp_Protocol_ListVersions, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_UploadVersion(CStartUploadVersion &&_Params)
		-> TCContinuation<CStartUploadVersion::CResult>
	{
		return mp_Server(&CVersionManagerDaemonActor::CServer::fp_Protocol_UploadVersion, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_DownloadVersion(CStartDownloadVersion &&_Params)
		-> TCContinuation<CStartDownloadVersion::CResult>
	{
		return mp_Server(&CVersionManagerDaemonActor::CServer::fp_Protocol_DownloadVersion, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_SubscribeToUpdates(CSubscribeToUpdates &&_Params)
		-> TCContinuation<CSubscribeToUpdates::CResult> 
	{
		return mp_Server(&CVersionManagerDaemonActor::CServer::fp_Protocol_SubscribeToUpdates, fg_GetCallingHostInfo(), fg_Move(_Params));
	}
	
	void CVersionManagerDaemonActor::CServer::fp_Publish()
	{
		mp_ProtocolImplementation = fg_ConstructDistributedActor<CVersionManagerImplementation>(fg_ThisActor(this));
		
		auto &DistributionManager = NConcurrency::fg_GetDistributionManager();
		DistributionManager
			(
				&CActorDistributionManager::f_PublishActor
				, mp_ProtocolImplementation
				, "com.malterlib/Cloud/VersionManager"
				, NConcurrency::CDistributedActorInheritanceHeirarchyPublish::fs_GetHierarchy<CVersionManager>()
			)
			> [this] (TCAsyncResult<CDistributedActorPublication> &&_Publication)
			{
				mp_ProtocolPublication = fg_Move(*_Publication);
			}
		;
	}

	NException::CException CVersionManagerDaemonActor::CServer::fp_AccessDenied(CCallingHostInfo const &_CallingHostInfo, CStr const &_Description)
	{
		fsp_LogActivityWarning(_CallingHostInfo, fg_Format("Denied access to: {}", _Description));
		return DMibErrorInstance("Access denied");
	}
}
