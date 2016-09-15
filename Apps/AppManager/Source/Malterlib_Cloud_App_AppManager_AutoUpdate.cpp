// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Concurrency/Actor/Timer>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	CVersionManager::CVersionIdentifier CAppManagerActor::fp_FindVersionForAutoUpdate
		(
			TCSharedPointer<CApplication> const &_pApplication
			, TCSet<CStr> const &_RequiredTags
			, TCSet<CStr> const &_AllowedBranches
			, CStr &o_Error
		)
	{
		auto *pVersionManagerApp = mp_VersionManagerApplications.f_FindEqual(_pApplication->m_VersionManagerApplication);
		if (!pVersionManagerApp)
		{
			o_Error = fg_Format("No connected version managers provides versions for: {}", _pApplication->m_VersionManagerApplication); 
			return {};
		}
		decltype(pVersionManagerApp->m_VersionsByTime.f_GetIterator()) iVersion;
		auto &CurrentVersionID = _pApplication->m_LastInstalledVersion;
	
		CVersionManager::CVersionIdentifier VersionID;
		for (iVersion.f_StartBackward(pVersionManagerApp->m_VersionsByTime); iVersion; --iVersion)
		{
			auto &RemoteVersion = *iVersion;
			if (!_AllowedBranches.f_FindEqual(RemoteVersion.f_GetVersionID().m_Branch))
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
			break;
		}
		
		if (!VersionID.f_IsValid())
		{
			if (_RequiredTags.f_IsEmpty())
				o_Error = "No version in the same branch as the currently installed version was found";
			else
				o_Error = "No version with the required tags and in the same branch as the currently installed version was found";
			return {};
		}
		if (VersionID <= CurrentVersionID)
		{
			if (_RequiredTags.f_IsEmpty())
				o_Error = "No newer version in the same branch as the currently installed version was found";
			else
				o_Error = "No newer version with the required tags and in the same branch as the currently installed version was found";
			return {};
		}
		return VersionID;
	}
	
	void CAppManagerActor::fp_AutoUpdate_Update()
	{
		bool bReschedule = false;
		for (auto &pApplication : mp_Applications)
		{
			if (!pApplication->m_bAutoUpdate)
				continue;

			CStr Error;
			auto VersionID = fp_FindVersionForAutoUpdate(pApplication, pApplication->m_AutoUpdateTags, pApplication->m_AutoUpdateBranches, Error);
			if (!VersionID.f_IsValid())
				continue;
			
			if (pApplication->m_bOperationInProgress)
			{
				bReschedule = true;
				continue;
			}
			self
				(
					&CAppManagerActor::fp_CommandLine_UpdateApplication
					, CEJSON
					(
						{
							"Name"_= pApplication->m_Name 
							, "DryRun"_= false
							, "UpdateSettings"_= false
							, "Version"_= CStr::fs_ToStr(VersionID)
						}
					)
					, true 
				)
				> [this](TCAsyncResult<CDistributedAppCommandLineResults> &&_Results)
				{
					if (!_Results)
					{
						DMibLogWithCategory(Malterlib/Cloud/AppManager, Error, "Auto update of application failed: : {}", _Results.f_GetExceptionStr());
					}
				}
			;
		}
		if (bReschedule)
			mp_bPendingAutoUpdate = true;
		else
			mp_bPendingAutoUpdate = false;
	}
}
