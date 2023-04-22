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
	TCFuture<TCSet<CStr>> CVersionManagerDaemonActor::CServer::fp_FilterApplicationsByPermissions(CStr const &_Description, TCSet<CStr> const &_Applications)
	{
		TCPromise<TCSet<CStr>> Promise;
		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;
		Permissions["//ALL//"] = {{"Application/ReadAll", "Application/ListAll"}};
		for (auto &Application : _Applications)
			Permissions[Application] = {CPermissionQuery{fg_Format("Application/Read/{}", Application)}.f_Description("Access application {} in VersionManager"_f << Application)};

		mp_Permissions.f_HasPermissions(_Description, Permissions) > Promise / [Promise, _Applications](NContainer::TCMap<NStr::CStr, bool> const &_HasPermissions)
			{
				TCSet<CStr> Applications;
				bool bListAllAccess = _HasPermissions["//ALL//"];

				for (auto &Application : _Applications)
				{
					if (!bListAllAccess && !_HasPermissions[Application])
						continue;
					Applications[Application];
				}

				Promise.f_SetResult(fg_Move(Applications));
			}
		;
		return Promise.f_MoveFuture();
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListApplications(CListApplications &&_Params) -> TCFuture<CListApplications::CResult>
	{
		auto pThis = m_pThis;
		
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto Auditor = pThis->mp_AppState.f_Auditor();
		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		Auditor.f_Info("Listing applications");
		
		auto Applications = co_await (pThis->fp_FilterApplicationsByPermissions("List applications", pThis->fp_ApplicationSet()) % "Permission denied listing applications" % Auditor);

		Auditor.f_Info("Listed applications: {vs,vb}"_f << Applications);

		CVersionManager::CListApplications::CResult Results;
		Results.m_Applications = fg_Move(Applications);
		co_return fg_Move(Results);
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ListVersions(CListVersions &&_Params) -> TCFuture<CListVersions::CResult>
	{
		auto pThis = m_pThis;
		
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					if (pThis->f_IsDestroyed())
						return DMibErrorInstance("Shutting down");
					return {};
				}
			)
		;

		auto QueryFileActor = pThis->fp_GetQueryFileActor();

		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		Auditor.f_Info("Listing versions");
		
		TCSet<CStr> Applications;

		if (!_Params.m_ForApplication.f_IsEmpty())
		{
			if (!CVersionManager::fs_IsValidApplicationName(_Params.m_ForApplication))
				co_return Auditor.f_Exception("Invalid application format");
			
			Applications[_Params.m_ForApplication];
		}
		else
			Applications = pThis->fp_ApplicationSet();

		auto FilteredApplications = co_await (pThis->fp_FilterApplicationsByPermissions("List versions", Applications) % "Permission denied listing versions" % Auditor);

		if (!_Params.m_ForApplication.f_IsEmpty() && FilteredApplications.f_IsEmpty())
			co_return Auditor.f_AccessDenied("(List Versions)", {"Application/ReadAll", "Application/ListAll", "Application/Read/*"});

		CVersionManager::CListVersions::CResult Results;
		for (auto &ApplicationName : FilteredApplications)
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

		co_return fg_Move(Results);
	}
}
