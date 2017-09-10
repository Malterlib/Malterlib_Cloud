// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CVersionManager::CVersionIDAndPlatform CAppManagerActor::fp_FindVersion
		(
			TCSharedPointer<CApplication> const &_pApplication
			, TCSet<CStr> const &_RequiredTags
			, TCSet<CStr> const &_AllowedBranches
			, CStr const &_Platform
			, CStr &o_Error
			, EFindVersionFlag _Flags
			, CVersionManager::CVersionInformation &o_VersionInfo
		)
	{
		auto *pVersionManagerApp = mp_VersionManagerApplications.f_FindEqual(_pApplication->m_Settings.m_VersionManagerApplication);
		if (!pVersionManagerApp)
		{
			o_Error = fg_Format("No connected version managers provides versions for: {}", _pApplication->m_Settings.m_VersionManagerApplication); 
			return {};
		}
		decltype(pVersionManagerApp->m_VersionsByTime.f_GetIterator()) iVersion;
		auto &CurrentVersionID = _pApplication->m_LastInstalledVersionFinished;
		auto &CurrentVersionInfo = _pApplication->m_LastInstalledVersionInfoFinished;
		
		CVersionManager::CVersionIDAndPlatform VersionID;
		for (iVersion.f_StartBackward(pVersionManagerApp->m_VersionsByTime); iVersion; --iVersion)
		{
			auto &RemoteVersion = *iVersion;
			if (!_AllowedBranches.f_IsEmpty() && !_AllowedBranches.f_FindEqual(RemoteVersion.f_GetVersionID().m_VersionID.m_Branch))
			{
				bool bBranchAllowed = false;
				for (auto &AllowedBranch : _AllowedBranches)
				{
					if (NStr::fg_StrMatchWildcard(RemoteVersion.f_GetVersionID().m_VersionID.m_Branch.f_GetStr(), AllowedBranch.f_GetStr()) == EMatchWildcardResult_WholeStringMatchedAndPatternExhausted)
					{
						bBranchAllowed = true;
						break;
					}
				}
				if (!bBranchAllowed)
					continue;
			}
			if (RemoteVersion.f_GetVersionID().m_Platform != _Platform)
				continue;
			bool bFoundAllTags = true;
			for (auto &RequiredTag : _RequiredTags)
			{
				if (!RemoteVersion.m_VersionInfo.m_Tags.f_FindEqual(RequiredTag))
				{
					bFoundAllTags = false;
					break;
				}
			}
			if (!bFoundAllTags)
				continue;
			VersionID = RemoteVersion.f_GetVersionID();
			o_VersionInfo = RemoteVersion.m_VersionInfo; 
			break;
		}
		
		if (!VersionID.f_IsValid())
		{
			if (_Flags & EFindVersionFlag_ForAdd)
			{
				if (_AllowedBranches.f_IsEmpty())
				{
					if (_RequiredTags.f_IsEmpty())
						o_Error = "No version found for application";
					else
						o_Error = "No version with the required tags found for application";
				}
				else
				{
					if (_RequiredTags.f_IsEmpty())
						o_Error = "No version found for application in the allowed branches";
					else
						o_Error = "No version with the required tags found for application in the allowed branches";
				}
			}
			else
			{
				if (_RequiredTags.f_IsEmpty())
					o_Error = "No version in the same branch as the currently installed version was found";
				else
					o_Error = "No version with the required tags and in the same branch as the currently installed version was found";
			}
			return {};
		}

		if (_Flags & EFindVersionFlag_ForAdd)
			return VersionID;
		
		if 
			(
				VersionID.m_VersionID.m_Branch == CurrentVersionID.m_VersionID.m_Branch 
				&& 
				(
					VersionID < CurrentVersionID 
					|| 
					(
						VersionID == CurrentVersionID
						&& o_VersionInfo.m_Time == CurrentVersionInfo.m_Time
					)
				)
			 )
		{
			if (_RequiredTags.f_IsEmpty())
				o_Error = "No newer version in the same branch as the currently installed version was found";
			else
				o_Error = "No newer version with the required tags and in the same branch as the currently installed version was found";
			return {};
		}
		
		if 
			(
				! (_Flags & EFindVersionFlag_RetryFailed)
				&& _pApplication->m_LastTriedInstalledVersion.f_IsValid() 
				&& VersionID == _pApplication->m_LastTriedInstalledVersion 
				&& o_VersionInfo.m_Time == _pApplication->m_LastTriedInstalledVersionInfo.m_Time
				&& o_VersionInfo.m_RetrySequence == _pApplication->m_LastTriedInstalledVersionInfo.m_RetrySequence
			)
		{
			o_Error = "Already tried and failed to install this version";
			return {};
		}

		if (CurrentVersionInfo.m_Time.f_IsValid() && o_VersionInfo.m_Time < CurrentVersionInfo.m_Time)
		{
			o_Error = fg_Format("The selected version {} was older than the currently installed version", VersionID);
			return {};
		}

		return VersionID;
	}
	
	void CAppManagerActor::fp_AutoUpdate_Update()
	{
		if (mp_bPendingSelfUpdateInProgress || mp_State.m_bStoppingApp)
			return;
			
		bool bReschedule = false;
		for (auto &pApplication : mp_Applications)
		{
			if (!pApplication->m_Settings.m_bAutoUpdate)
				continue;

			CStr Error;
			auto AllowedBranches = pApplication->m_Settings.m_AutoUpdateBranches;
			if (AllowedBranches.f_IsEmpty())
				AllowedBranches = fg_CreateSet(pApplication->m_LastInstalledVersion.m_VersionID.m_Branch);

			CVersionManager::CVersionInformation VersionInfo;
			auto VersionID = fp_FindVersion
				(
					pApplication
					, pApplication->m_Settings.m_AutoUpdateTags
					, AllowedBranches
					, pApplication->m_LastInstalledVersion.m_Platform
					, Error
					, EFindVersionFlag_None
					, VersionInfo 
				)
			;
			
			if (!VersionID.f_IsValid())
			{
				DMibLogWithCategory(Malterlib/Cloud/AppManager, Debug, "No new version found for '{}': {}", pApplication->m_Name, Error);
				continue;
			}
			
			if (pApplication->f_IsInProgress())
			{
				bReschedule = true;
				continue;
			}
			
			CAppManagerInterface::CApplicationUpdate Update;
			Update.m_Version = VersionID.m_VersionID;
			Update.m_Platform = pApplication->m_LastInstalledVersion.m_Platform;
			
			auto CallingHostInfoScope = fp_PopulateCurrentHostInfoIfMissing("AutoUpdate");
			
			fp_UpdateApplication
				(
					pApplication->m_Name
					, Update
					, VersionInfo
					, {}
					, [Name = pApplication->m_Name](CStr const &_Info)
					{
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Auto update application '{}': {}", Name, _Info);
					}
					, false 
				)
				> [](TCAsyncResult<> &&_Results) mutable
				{
					if (!_Results)
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Auto update of application failed. {}", _Results.f_GetExceptionStr());
				}
			;
		}
		mp_bPendingAutoUpdate = bReschedule;
	}
}
