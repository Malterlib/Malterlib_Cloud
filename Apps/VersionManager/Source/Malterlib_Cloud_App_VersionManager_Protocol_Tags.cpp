// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Daemon/Daemon>
#include <Mib/Concurrency/DistributedActor>
#include <Mib/Concurrency/DistributedActorTrustManager>
#include <Mib/Concurrency/DistributedActorTrustManagerDatabases/JsonDirectory>
#include <Mib/Concurrency/ActorSubscription>

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
			mp_AppState.m_TrustManager(&CDistributedActorTrustManager::f_RegisterPermissions, NewPermissions).f_DiscardResult();
	}

	TCFuture<CVersionManagerDaemonActor::CServer::CFilteredTagsResult> CVersionManagerDaemonActor::CServer::fp_FilterTags
		(
			CStr _Description
			, TCSet<CStr> _TagsAdded
			, TCSet<CStr> _TagsRemoved
		)
	{
		TCMap<CStr, TCVector<CPermissionQuery>> Permissions;

		Permissions["//TagAll//"] = {{"Application/TagAll"}};
		TCSet<CStr> DeniedTags;
		for (auto &Tag : _TagsAdded)
		{
			if (CVersionManager::fs_IsValidTag(Tag))
				Permissions[Tag] = {CPermissionQuery{fg_Format("Application/Tag/{}", Tag)}.f_Description("Access to tag {} in VersionManager"_f << Tag)};
			else
				DeniedTags[Tag];
		}
		for (auto &Tag : _TagsRemoved)
		{
			if (CVersionManager::fs_IsValidTag(Tag))
				Permissions[Tag] = {CPermissionQuery{fg_Format("Application/Tag/{}", Tag)}.f_Description("Access to tag {} in VersionManager"_f << Tag)};
			else
				DeniedTags[Tag];
		}

		auto HasPermissions = co_await mp_Permissions.f_HasPermissions(_Description, Permissions);
		CFilteredTagsResult Result;
		Result.m_DeniedTags = DeniedTags;

		if (HasPermissions["//TagAll//"])
		{
			Result.m_TagsAdded = _TagsAdded;
			Result.m_TagsRemoved = _TagsRemoved;
		}
		else
		{
			for (auto &Tag : _TagsAdded)
			{
				auto pHasPermission = HasPermissions.f_FindEqual(Tag);
				if (pHasPermission && *pHasPermission)
					Result.m_TagsAdded[Tag];
				else
					Result.m_DeniedTags[Tag];
			}
			for (auto &Tag : _TagsRemoved)
			{
				auto pHasPermission = HasPermissions.f_FindEqual(Tag);
				if (pHasPermission && *pHasPermission)
					Result.m_TagsRemoved[Tag];
				else
					Result.m_DeniedTags[Tag];
			}
		}

		co_return fg_Move(Result);
	}

	auto CVersionManagerDaemonActor::CServer::CVersionManagerImplementation::f_ChangeTags(CChangeTags _Params) -> TCFuture<CChangeTags::CResult>
	{
		auto pThis = m_pThis;
		
		auto OnResume = co_await pThis->f_CheckDestroyedOnResume();

		auto Auditor = pThis->mp_AppState.f_Auditor();
		
		if (!CVersionManager::fs_IsValidApplicationName(_Params.m_Application))
			co_return Auditor.f_Exception({"Invalid application format", "(change tags)"});

		{
			CStr ErrorStr;
			if (!CVersionManager::fs_IsValidVersionIdentifier(_Params.m_VersionID, ErrorStr))
				co_return Auditor.f_Exception({fg_Format("Invalid version ID format: {}", ErrorStr), "(change tags)"});
		}

		auto FilteredTags = co_await
			(
				pThis->fp_FilterTags("Change tags in the version manager", _Params.m_AddTags, _Params.m_RemoveTags)
				% "Access denied filtering tags by permission"
				% Auditor
			)
		;

		TCSet<CStr> const &DeniedTags = FilteredTags.m_DeniedTags;
		TCSet<CStr> const &AddTags = FilteredTags.m_TagsAdded;
		TCSet<CStr> const &RemoveTags = FilteredTags.m_TagsRemoved;

		if (AddTags.f_IsEmpty() && RemoveTags.f_IsEmpty())
		{
			if (!DeniedTags.f_IsEmpty())
				co_return Auditor.f_AccessDenied({fg_Format("Access denied to all tags specified: {vs,vb}", DeniedTags), "(Change tags)"}, {"Application/TagAll", "Application/Tag/*"});

			if (!_Params.m_bIncreaseRetrySequence)
			{
				Auditor.f_Info("Change tags resulted in no changed tags");
				CVersionManager::CChangeTags::CResult Result;
				co_return Result;
			}
		}

		auto *pApplication = pThis->mp_Applications.f_FindEqual(_Params.m_Application);
		if (!pApplication)
			co_return Auditor.f_Exception(fg_Format("No such application: {}", _Params.m_Application));

		TCFutureVector<CSizeInfo> VersionResults;
		TCFutureVector<void> DatabaseResults;

		auto fTagVersion = [&](CVersion &_Version)
			{
				CStr ApplicationDirectory = fg_Format("{}/Applications", pThis->mp_AppState.m_RootDirectory);
				CStr VersionPath = fg_Format("{}/{}/{}", ApplicationDirectory, _Params.m_Application, _Version.f_GetIdentifier().f_EncodeFileName());

				_Version.m_VersionInfo.m_Tags -= RemoveTags;
				_Version.m_VersionInfo.m_Tags += AddTags;
				if (_Params.m_bIncreaseRetrySequence)
					++_Version.m_VersionInfo.m_RetrySequence;

				pThis->fp_NewVersion(_Params.m_Application, _Version);
				pThis->fp_SaveVersionInfo(VersionPath, _Version.m_VersionInfo) > VersionResults;
				pThis->fp_SaveVersionToDatabase(_Params.m_Application, _Version.f_GetIdentifier(), _Version.m_VersionInfo) > DatabaseResults;
			}
		;

		if (!_Params.m_Platform.f_IsEmpty())
		{
			CVersionManager::CVersionIDAndPlatform VersionIDAndPlatform;
			VersionIDAndPlatform.m_VersionID = _Params.m_VersionID;
			VersionIDAndPlatform.m_Platform = _Params.m_Platform;
			auto *pVersion = pApplication->m_Versions.f_FindEqual(VersionIDAndPlatform);
			if (!pVersion)
				co_return Auditor.f_Exception(fg_Format("No such version: {}", VersionIDAndPlatform));
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

		auto Results = co_await fg_AllDoneWrapped(VersionResults).f_Wrap();
		if (!Results)
			co_return Auditor.f_Exception({"Failed to save version infos. See Version Manager log.", fg_Format("Error: {}", Results.f_GetExceptionStr())});

		auto DbResults = co_await fg_AllDoneWrapped(DatabaseResults).f_Wrap();
		if (!DbResults)
			Auditor.f_Warning(fg_Format("Failed to save version infos to database: {}", DbResults.f_GetExceptionStr()));

		bool bFailed = false;
		mint nVersions = 0;
		for (auto &Version : *Results)
		{
			if (!Version)
			{
				Auditor.f_Error(fg_Format("Failed to save version info: {}", Version.f_GetExceptionStr()));
				bFailed = true;
			}
			else
				++nVersions;
		}

		if (DbResults)
		{
			for (auto &DbResult : *DbResults)
			{
				if (!DbResult)
					Auditor.f_Warning(fg_Format("Failed to save version to database: {}", DbResult.f_GetExceptionStr()));
			}
		}

		Auditor.f_Info
			(
				"Changed tags for {} {} {}   Removed {vs,vb}   Added {vs,vb}   affected {} versions"_f
				<< _Params.m_Application
				<< _Params.m_VersionID
				<< _Params.m_Platform
				<< RemoveTags
				<< AddTags
				<< nVersions
			)
		;

		if (bFailed)
			co_return DMibErrorInstance("Failed to save one or more version infos. Consult version manager log files for more info.");

		CVersionManager::CChangeTags::CResult Result;
		Result.m_DeniedTags = DeniedTags;

		co_return fg_Move(Result);
	}
}
