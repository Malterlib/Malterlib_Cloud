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
	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_FilterApplicationsByPermissions(CStr const &_CallingHostID, TCSet<CStr> const &_Applications)
	{
		TCSet<CStr> Applications;

		bool bListAllAccess = mp_Permissions.f_HostHasAnyPermission(_CallingHostID, "Application/ReadAll", "Application/ListAll");
		
		for (auto &Application : _Applications)
		{
			if (!bListAllAccess && !mp_Permissions.f_HostHasPermission(_CallingHostID, fg_Format("Application/Read/{}", Application)))
				continue;
			Applications[Application];
		}
		
		return Applications;
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListApplications(CListApplications &&_Params) -> TCContinuation<CListApplications::CResult> 
	{
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");

		auto Auditor = pThis->mp_AppState.f_Auditor();
		NConcurrency::TCContinuation<CVersionManager::CListApplications::CResult> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		Auditor.f_Info("Listing applications");
		
		CVersionManager::CListApplications::CResult Results;
		Results.m_Applications = pThis->fp_FilterApplicationsByPermissions(fg_GetCallingHostID(), pThis->fp_ApplicationSet());

		Auditor.f_Info(fg_Format("Listed applications: {vs,vb}", Results.m_Applications));
		
		Continuation.f_SetResult(fg_Move(Results));
		return Continuation;
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListVersions(CListVersions &&_Params) -> TCContinuation<CListVersions::CResult>
	{
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		NConcurrency::TCContinuation<CVersionManager::CListVersions::CResult> Continuation;
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		Auditor.f_Info("Listing versions");
		
		TCSet<CStr> Applications;
		
		CStr CallingHostID = fg_GetCallingHostID();
		
		if (!_Params.m_ForApplication.f_IsEmpty())
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Params.m_ForApplication))
				return Auditor.f_Exception("Invalid application format");
			
			Applications[_Params.m_ForApplication];
			if (pThis->fp_FilterApplicationsByPermissions(CallingHostID, Applications).f_IsEmpty())
				return Auditor.f_AccessDenied("(List Versions)");
		}
		else
			Applications = pThis->fp_FilterApplicationsByPermissions(CallingHostID, pThis->fp_ApplicationSet());
			
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
		
		Auditor.f_Info(fg_Format("Listed versions: {vs,vb}", VersionsText));
		
		Continuation.f_SetResult(fg_Move(Results));

		return Continuation;
	}
}
