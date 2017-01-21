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

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ChangeTags(CChangeTags &&_Params) -> TCContinuation<CChangeTags::CResult> 
	{
		auto &CallingHostInfo = fg_GetCallingHostInfo();
		auto pThis = m_pThis;
		
		if (!pThis->mp_pCanDestroyTracker)
			return DMibErrorInstance("Shutting down");
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
		{
			CStr Error = "Invalid application format";
			fsp_LogActivityError(CallingHostInfo, Error + " (change tags)");
			return DMibErrorInstance(Error);
		}

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionID, ErrorStr))
			{
				CStr Error = fg_Format("Invalid version ID format: {}", ErrorStr);
				fsp_LogActivityError(CallingHostInfo, Error + " (change tags)");
				return DMibErrorInstance(Error);
			}
		}
		
		TCSet<CStr> DeniedTags;
		TCSet<CStr> AddTags = pThis->fp_FilterTags(CallingHostInfo.f_GetRealHostID(), _Params.m_AddTags, DeniedTags);
		TCSet<CStr> RemoveTags = pThis->fp_FilterTags(CallingHostInfo.f_GetRealHostID(), _Params.m_RemoveTags, DeniedTags);
		
		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
		{
			if (!DeniedTags.f_IsEmpty())
				return pThis->fp_AccessDenied(CallingHostInfo, "Change tags", fg_Format("Access denied to all tags specified: {vs,vb}", DeniedTags));
			fsp_LogActivityInfo(CallingHostInfo, "Change tags resulted in no changed tags");
			CVersionManager::CChangeTags::CResult Result;
			return fg_Explicit(Result);
		}

		auto *pApplication = pThis->mp_Applications.f_FindEqual(_Params.m_Application);
		if (!pApplication)
		{
			CStr Error = fg_Format("No such application: {}", _Params.m_Application);
			fsp_LogActivityError(CallingHostInfo, Error);
			return DMibErrorInstance(Error);
		}

		TCActorResultVector<CSizeInfo> VersionResults;
		
		auto fTagVersion = [&](CVersion &_Version)
			{
				CStr ApplicationDirectory = fg_Format("{}/Applications", CFile::fs_GetProgramDirectory());
				CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Version.f_GetIdentifier().f_EncodeFileName());
				
				_Version.m_VersionInfo.m_Tags -= RemoveTags;
				_Version.m_VersionInfo.m_Tags += AddTags;
				
				pThis->fp_NewVersion(_Params.m_Application, _Version); 
				pThis->fp_SaveVersionInfo(pThis->fp_GetQueryFileActor(), VersionPath, _Version.m_VersionInfo) > VersionResults.f_AddResult(); 
			}
		;

		if (!_Params.m_Platform.f_IsEmpty())
		{
			CVersionManager::CVersionIDAndPlatform VersionIDAndPlatform;
			VersionIDAndPlatform.m_VersionID = _Params.m_VersionID;
			VersionIDAndPlatform.m_Platform = _Params.m_Platform;
			auto *pVersion = pApplication->m_Versions.f_FindEqual(VersionIDAndPlatform);
			if (!pVersion)
			{
				CStr Error = fg_Format("No such version: {}", VersionIDAndPlatform);
				fsp_LogActivityError(CallingHostInfo, Error);
				return DMibErrorInstance(Error);
			}
			fTagVersion(*pVersion);
		}
		else
		{
			CVersionManager::CVersionIDAndPlatform VersionIDAndPlatform;
			VersionIDAndPlatform.m_VersionID = _Params.m_VersionID;
			for (auto iVersion = pApplication->m_Versions.f_GetIterator_SmallestGreaterThanEqual(VersionIDAndPlatform); iVersion; ++iVersion)
			{
				if (iVersion.f_GetKey().m_VersionID != _Params.m_VersionID)
					break;
				fTagVersion(*iVersion);
			}
		}
		
		TCContinuation<CVersionManager::CChangeTags::CResult> Continuation;
		
		VersionResults.f_GetResults() > 
			[
				Continuation
				, DeniedTags
				, ApplicationName = _Params.m_Application
				, VersionID = _Params.m_VersionID
				, Platform = _Params.m_Platform
				, AddTags
				, RemoveTags
				, CallingHostInfo
			]
			(TCAsyncResult<TCVector<TCAsyncResult<CSizeInfo>>> &&_Results)
			{
				if (!_Results)
				{
					fsp_LogActivityError(CallingHostInfo, fg_Format("Failed to save version infos: {}", _Results.f_GetExceptionStr()));
					Continuation.f_SetException(DMibErrorInstance("Failed to save version infos. Consult version manager log files for more info."));
					return;
				}
				
				bool bFailed = false;
				for (auto &Version : *_Results)
				{
					if (!Version)
					{
						fsp_LogActivityError(CallingHostInfo, fg_Format("Failed to save version info: {}", Version.f_GetExceptionStr()));
						bFailed = true;
					}
				}

				fsp_LogActivityInfo(CallingHostInfo, fg_Format("Changed tags for {} {} {}   Removed {vs,vb}   Added {vs,vb}", ApplicationName, VersionID, Platform, AddTags, RemoveTags));
				
				if (bFailed)
				{
					Continuation.f_SetException(DMibErrorInstance("Failed to save one or more version infos. Consult version manager log files for more info."));
					return;
				}
					
				CVersionManager::CChangeTags::CResult Result;
				Result.m_DeniedTags = DeniedTags;
				Continuation.f_SetResult(fg_Move(Result));
			}
		;
		return Continuation;
	}
}
