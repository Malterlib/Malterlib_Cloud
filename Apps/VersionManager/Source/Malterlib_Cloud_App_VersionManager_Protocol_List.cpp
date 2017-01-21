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
	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_FilterApplicationsByPermissions(CCallingHostInfo const &_CallingHostInfo, TCSet<CStr> const &_Applications)
	{
		TCSet<CStr> Applications;

		bool bListAllAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostInfo.f_GetRealHostID(), "Application/ReadAll", "Application/ListAll");
		
		for (auto &Application : _Applications)
		{
			if (!bListAllAccess && !mp_Permissions.f_HostHasPermission(_CallingHostInfo.f_GetRealHostID(), fg_Format("Application/Read/{}", Application)))
				continue;
			Applications[Application];
		}
		
		return Applications;
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListApplications(CListApplications &&_Params) -> TCContinuation<CListApplications::CResult> 
	{
		auto &CallingHostInfo = fg_GetCallingHostInfo();
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
			
		NConcurrency::TCContinuation<CVersionManager::CListApplications::CResult> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		fsp_LogActivityInfo(CallingHostInfo, "Listing applications");
		
		CVersionManager::CListApplications::CResult Results;
		Results.m_Applications = pThis->fp_FilterApplicationsByPermissions(CallingHostInfo, pThis->fp_ApplicationSet());

		fsp_LogActivityInfo(CallingHostInfo, fg_Format("Listed applications: {vs,vb}", Results.m_Applications));
		
		Continuation.f_SetResult(fg_Move(Results));
		return Continuation;
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListVersions(CListVersions &&_Params) -> TCContinuation<CListVersions::CResult>
	{
		auto &CallingHostInfo = fg_GetCallingHostInfo();
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		NConcurrency::TCContinuation<CVersionManager::CListVersions::CResult> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		fsp_LogActivityInfo(CallingHostInfo, "Listing versions");
		
		TCSet<CStr> Applications;
		
		if (!_Params.m_ForApplication.f_IsEmpty())
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Params.m_ForApplication))
			{
				CStr Error = "Invalid application format";
				fsp_LogActivityError(CallingHostInfo, Error);
				return DMibErrorInstance(Error);
			}
			
			Applications[_Params.m_ForApplication];
			if (pThis->fp_FilterApplicationsByPermissions(CallingHostInfo, Applications).f_IsEmpty())
				return pThis->fp_AccessDenied(CallingHostInfo, "List Versions");
		}
		else
			Applications = pThis->fp_FilterApplicationsByPermissions(CallingHostInfo, pThis->fp_ApplicationSet());
			
		CVersionManager::CListVersions::CResult Results;
		for (auto &ApplicationName : Applications)
		{
			auto *pApplication = pThis->mp_Applications.f_FindEqual(ApplicationName);
			if (!pApplication)
				continue;
			auto &Application = *pApplication;
			auto &OutVersions = Results.m_Versions[ApplicationName];
			for (auto &Version : Application.m_Versions)
				OutVersions[Version.f_GetIdentifier()] = Version.m_VersionInfo;
		}

		TCMap<CStr, CStr> VersionsText;
		for (auto &Application : Results.m_Versions)
			VersionsText[Results.m_Versions.fs_GetKey(Application)] = fg_Format("{} versions", Application.f_GetLen());
		
		fsp_LogActivityInfo(CallingHostInfo, fg_Format("Listed versions: {vs,vb}", VersionsText));
		
		Continuation.f_SetResult(fg_Move(Results));

		return Continuation;
	}
}
