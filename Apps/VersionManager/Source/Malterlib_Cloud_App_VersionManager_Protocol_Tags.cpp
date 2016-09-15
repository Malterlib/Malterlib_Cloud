// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JSONDirectory>
#include <Mib/Concurrency/ActorCallbackManager>

#include "Malterlib_Cloud_App_VersionManager.h"
#include "Malterlib_Cloud_App_VersionManager_Server.h"

namespace NMib::NCloud::NVersionManager
{
	void CVersionManagerDaemonActor::CServer::fp_NewTagsKnown(TCSet<CStr> const &_Tags)
	{
		TCSet<CStr> NewPermissions;
		for (auto &Tag : _Tags)
		{
			if (mp_KnownTags(Tag).f_WasCreated())
				NewPermissions[fg_Format("Application/Tag/{}", Tag)];
		}
		if (!NewPermissions.f_IsEmpty())
			mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, NewPermissions) > fg_DiscardResult();
	}

	TCSet<CStr> CVersionManagerDaemonActor::CServer::fp_FilterTags(CStr const &_HostID, TCSet<CStr> const &_Tags, TCSet<CStr> &o_DeniedTags)
	{
		if (mp_Permissions.f_HostHasAnyPermission(_HostID, "Application/TagAll"))
			return _Tags;
		
		TCSet<CStr> Tags;
		for (auto &Tag : _Tags)
		{
			if (CVersionManager::fs_IsValidTag(Tag) && mp_Permissions.f_HostHasPermission(_HostID, fg_Format("Application/Tag/{}", Tag)))
				Tags[Tag];
			else
				o_DeniedTags[Tag];
		}
		return Tags;
	}

	auto CVersionManagerDaemonActor::CServer::fp_Protocol_ChangeTags(CCallingHostInfo const &_CallingHostInfo, CVersionManager::CChangeTags &&_Params)
		-> TCContinuation<CVersionManager::CChangeTags::CResult> 
	{
		if (!mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
		{
			CStr Error = "Invalid application format";
			fsp_LogActivityError(_CallingHostInfo, Error + " (change tags)");
			return DMibErrorInstance(Error);
		}

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionID, ErrorStr))
			{
				CStr Error = fg_Format("Invalid version ID format: {}", ErrorStr);
				fsp_LogActivityError(_CallingHostInfo, Error + " (change tags)");
				return DMibErrorInstance(Error);
			}
		}
		
		TCSet<CStr> DeniedTags;
		TCSet<CStr> AddTags = fp_FilterTags(_CallingHostInfo.f_GetRealHostID(), _Params.m_AddTags, DeniedTags);
		TCSet<CStr> RemoveTags = fp_FilterTags(_CallingHostInfo.f_GetRealHostID(), _Params.m_RemoveTags, DeniedTags);
		
		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
		{
			if (!DeniedTags.f_IsEmpty())
				return fp_AccessDenied(_CallingHostInfo, "Change tags", fg_Format("Access denied to all tags specified: {vs,vb}", DeniedTags));
			fsp_LogActivityInfo(_CallingHostInfo, "Change tags resulted in no changed tags");
			CVersionManager::CChangeTags::CResult Result;
			return fg_Explicit(Result);
		}

		auto *pApplication = mp_Applications.f_FindEqual(_Params.m_Application);
		if (!pApplication)
		{
			CStr Error = fg_Format("No such application: {}", _Params.m_Application);
			fsp_LogActivityError(_CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		auto *pVersion = pApplication->m_Versions.f_FindEqual(_Params.m_VersionID);
		if (!pVersion)
		{
			CStr Error = fg_Format("No such version: {}", _Params.m_VersionID);
			fsp_LogActivityError(_CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}
		
		CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
		CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Params.m_VersionID.f_EncodeFileName());
		
		pVersion->m_VersionInfo.m_Tags -= RemoveTags;
		pVersion->m_VersionInfo.m_Tags += AddTags;
		
		fp_NewVersion(_Params.m_Application, pVersion->f_GetIdentifier(), pVersion->m_VersionInfo); 
		
		TCContinuation<CVersionManager::CChangeTags::CResult> Continuation;
		
		self(&CServer::fp_SaveVersionInfo, fp_GetQueryFileActor(), VersionPath, pVersion->m_VersionInfo) 
			> [this, Continuation, DeniedTags, ApplicationName = _Params.m_Application, VersionID = _Params.m_VersionID, AddTags, RemoveTags, _CallingHostInfo]
			(TCAsyncResult<CSizeInfo> &&_Result)
			{
				if (!_Result)
				{
					fsp_LogActivityError(_CallingHostInfo, fg_Format("Failed to save version info: {}", _Result.f_GetExceptionStr()));
					Continuation.f_SetException(DMibErrorInstance("Failed to save version info. Consult version manager log files for more info."));
					return;
				}
				fsp_LogActivityInfo(_CallingHostInfo, fg_Format("Changed tags for {} {}   Removed {vs,vb}   Added {vs,vb}", ApplicationName, VersionID, AddTags, RemoveTags));
				CVersionManager::CChangeTags::CResult Result;
				Result.m_DeniedTags = DeniedTags;
				
				Continuation.f_SetResult(fg_Move(Result));
			}
		;
		
		return Continuation;
	}
}
