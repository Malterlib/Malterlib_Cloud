// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Encoding/JSONShortcuts>
#include <Mib/Cryptography/RandomID>
#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	ch8 const *CAppManagerActor::fsp_UpdateTypeToStr(EDistributedAppUpdateType _UpdateType)
	{
		switch (_UpdateType)
		{
		case EDistributedAppUpdateType_Independent: return "independent";
		case EDistributedAppUpdateType_AllAtOnce: return "all at once";
		case EDistributedAppUpdateType_OneAtATime: return "one at a time";
		default: DNeverGetHere; return "unknown";
		}
	}
	
	NConcurrency::TCContinuation<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Update(NStr::CStr const &_Name, CApplicationUpdate const &_Update)
	{
		return m_pThis->fp_UpdateApplication
			(
				_Name
				, _Update
				, {} 
				, {}
				, [_Name](CStr const &_Info) 
				{
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Update application '{}': {}", _Name, _Info);
				}
			)
		;
	}
	
	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_CancelAllUpdates(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		for (auto &pUpdateWeak : mp_RunningUpdates)
		{
			auto pUpdate = pUpdateWeak.f_Lock();
			if (pUpdate)
				pUpdate->m_bCancel = true;
		}
		
		fp_OnAppUpdateInfoChange();
		
		return fg_Explicit();
	}
	
	TCContinuation<uint32> CAppManagerActor::fp_CommandLine_UpdateApplication(CEJSON const &_Params, NPtr::TCSharedPointer<CCommandLineControl> const &_pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();
		
		TCContinuation<uint32> Continuation;
		
		CStr Package;
		CAppManagerInterface::CApplicationUpdate Update;
		
		if (auto *pValue = _Params.f_GetMember("Package"))
		{
			Package = _Params["Package"].f_String();
			if (Package.f_IsEmpty())
				return DMibErrorInstance("You have to specify a package");
			Package = CFile::fs_GetExpandedPath(CFile::fs_GetFullPath(Package, mp_State.m_RootDirectory));
		}
		else
		{
			Update.m_bDryRun = _Params["DryRun"].f_Boolean();
			Update.m_bUpdateSettings = _Params["UpdateSettings"].f_Boolean();
			
			CStr Version = _Params["Version"].f_String();
			if (auto *pValue = _Params.f_GetMember("VersionManagerPlatform"))
				Update.m_Platform = pValue->f_String(); 

			if (auto *pValue = _Params.f_GetMember("RequiredTags"))
			{
				TCSet<CStr> RequiredTags;
				for (auto &TagJSON : pValue->f_Array())
					RequiredTags[TagJSON.f_String()];
				Update.m_RequireTags = fg_Move(RequiredTags);
			}
			
			if (!Version.f_IsEmpty())
			{
				CStr ErrorStr;
				CVersionManager::CVersionID VersionID;
				if (!CVersionManager::fs_IsValidVersionIdentifier(Version, ErrorStr, &VersionID))
					return DMibErrorInstance(fg_Format("Invalid version ID format: {}", ErrorStr));
				Update.m_Version = VersionID;
			}
		}
		
		fp_UpdateApplication
			(
				Name
				, Update
				, {} 
				, Package
				, [=](CStr const &_Info)
				{
					*_pCommandLine += _Info + DMibNewLine;
					DMibLogWithCategory(Malterlib/Cloud/AppManager, Info, "Update application '{}': {}", Name, _Info);
				}
			)
			> [=](TCAsyncResult<> &&_Result)
			{
				Continuation.f_SetResult(_pCommandLine->f_AddAsyncResult(_Result));
			}
		;
		return Continuation;
	}
	
	TCContinuation<void> CAppManagerActor::fp_UpdateApplication
		(
			CStr const &_Name
			, CAppManagerInterface::CApplicationUpdate const &_Update
			, CVersionManager::CVersionInformation const &_VersionInfo
			, CStr const &_FromFileName
			, TCFunction<void (CStr const &_Info)> &&_fOnInfo
			, bool _bCheckPermissions
		)
	{
		auto Auditor = f_Auditor();

		if (_bCheckPermissions)
		{
			auto CallingHostID = fg_GetCallingHostID();
			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationUpdate"))
				return Auditor.f_AccessDenied("(Application update, command)");

			if (!mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)))
				return Auditor.f_AccessDenied("(Application update, app name)");
		}
		
		auto pOldApplication = mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));
		
		TCSharedPointer<CApplication> pApplication = *pOldApplication;
		
		if (pApplication->f_IsInProgress())
			return Auditor.f_Exception("Operation already in progress for application");

		bool bDownloadVersion = true;
		bool bUpdateSettings = false;
		bool bDryRun = false;
		CStr Package;
		CStr Platform = pApplication->m_LastInstalledVersion.m_Platform;
		TCSet<CStr> RequiredTags;
		CAppManagerInterface::CVersionIDAndPlatform VersionID;
		CAppManagerInterface::CVersionInformation VersionInfo;
		if (!_FromFileName.f_IsEmpty())
			bDownloadVersion = false;
		else
		{
			bDryRun = _Update.m_bDryRun;
			bUpdateSettings = _Update.m_bUpdateSettings; 
			if (_Update.m_Platform)
				Platform = *_Update.m_Platform;

			if (!CVersionManager::fs_IsValidPlatform(Platform))
				return Auditor.f_Exception("Invalid version platform format");
			
			auto RequiredTags = pApplication->m_Settings.m_AutoUpdateTags;
			auto AllowedBranches = pApplication->m_Settings.m_AutoUpdateBranches;
			
			if (_Update.m_RequireTags)
			{
				for (auto &Tag : *_Update.m_RequireTags)
				{
					if (!CVersionManager::fs_IsValidTag(Tag))
						return Auditor.f_Exception(fg_Format("'{}' is not a valid tag", Tag));
				}
				RequiredTags = *_Update.m_RequireTags;
			}
			
			if (AllowedBranches.f_IsEmpty())
				AllowedBranches = fg_CreateSet(pApplication->m_LastInstalledVersion.m_VersionID.m_Branch);
			
			if (_Update.m_Version)
			{
				CStr ErrorStr;
				VersionID.m_Platform = Platform;
				if (!_Update.m_Version->f_IsValid())
					return Auditor.f_Exception(fg_Format("Invalid version: {}", *_Update.m_Version));
				VersionID.m_VersionID = *_Update.m_Version;
				VersionInfo = _VersionInfo;
			}
			else
			{
				CStr Error;
				VersionID = fp_FindVersion(pApplication, RequiredTags, AllowedBranches, Platform, Error, EFindVersionFlag_RetryFailed, VersionInfo);
				if (!VersionID.f_IsValid())
					return Auditor.f_Exception(Error);
			}
		}
		
		if (VersionID.f_IsValid() && !VersionInfo.m_Time.f_IsValid())
		{
			if (pApplication->m_LastInstalledVersionFinished == VersionID)
				return Auditor.f_Exception("This version is already installed");
		}
		
		TCSharedPointer<NTime::CClock> pClock = fg_Construct(true);
		
		TCSharedPointerSupportWeak<CUpdateApplicationState> pState = fg_Construct();
		pState->m_pApplication = pApplication;
		pState->m_fOnInfo = fg_Move(_fOnInfo);
		pState->m_VersionID = VersionID;
		pState->m_VersionTime = VersionInfo.m_Time;
		pState->m_VersionRetrySequence = VersionInfo.m_RetrySequence;
		pState->m_RequiredTags = RequiredTags;
		pState->m_bDryRun = bDryRun;
		pState->m_Auditor = Auditor;
		pState->m_bUpdateSettings = bUpdateSettings;
		pState->m_pClock = pClock;
		if (!bDownloadVersion)
			pState->m_SourcePath = _FromFileName;
		pState->m_pInProgressScope = pApplication->f_SetInProgress();
		
		mp_RunningUpdates[pState];
		pState->m_pCleanupStateMap = g_OnScopeExitActor > [this, pStateWeak = pState.f_Weak()]
			{
				mp_RunningUpdates.f_Remove(pStateWeak);
				if (mp_RunningUpdates.f_IsEmpty() && !mp_CancelRunningUpdatesOnStopAppManagerContinuation.f_IsSet())
					mp_CancelRunningUpdatesOnStopAppManagerContinuation.f_SetResult();
			}
		;
		
		TCContinuation<void> Continuation;

		pState->m_fOnInfo(fg_Format("Starting update with type '{}'", fsp_UpdateTypeToStr(pApplication->f_GetUpdateType())));
		
		fp_UpdateApplicationRunProcess(pState) > [=](TCAsyncResult<void> &&_Result)
			{
				if (_Result)
				{
					Auditor.f_Info(fg_Format("Application '{}' updated successfully", pState->m_pApplication->m_Name));
					Continuation.f_SetResult();
					return;
				}

				CStr Error = _Result.f_GetExceptionStr();
				
				if (Error.f_Find("AppManager stopped") >= 0 && pState->m_pApplication->m_UpdateStage <= EUpdateStage::EUpdateStage_SyncStart)
				{
					Continuation.f_SetException(Auditor.f_Exception("Reschedule update after next restart"));
					return; // Ignore waiting updates when restarting
				}
				else
					fp_OnUpdateEvent(pState, EUpdateStage::EUpdateStage_Failed, Error) > fg_DiscardResult();
				
				Continuation.f_SetException(Auditor.f_Exception(Error));
				bool bUnencrypted = pState->m_bUnencrypted;
				
				if (!pApplication->m_bDeleted && bUnencrypted)
				{
					fp_RunUpdateScript(pApplication, EUpdateScript_OnError, Error, VersionID, pState->m_pVersionInfo.f_Get(), pClock->f_GetTime()) > [](TCAsyncResult<void> &&_Result)
						{
							if (!_Result)
							{
								DMibLogWithCategory
									(
										Malterlib/Cloud/AppManager
										, Warning
										, "Error script failed: {}"
										, _Result.f_GetExceptionStr()
									)
								;
							}
						}
					;
				}
				else
				{
					DMibLogWithCategory
						(
							Malterlib/Cloud/AppManager
							, Info
							, "Skipped error script: bDeleted {}   bUnencrypted {}"
							, pApplication->m_bDeleted  
							, bUnencrypted  
						)
					;
				}
			}
		;
		
		return Continuation;
	}

	TCContinuation<void> CAppManagerActor::fp_CancelAllApplicationUpdatesOnStopAppManager()
	{
		if (mp_RunningUpdates.f_IsEmpty())
			return fg_Explicit();
		
		TCContinuation<void> Continuation;
		
		mp_CancelRunningUpdatesOnStopAppManagerContinuation = Continuation;
		
		for (auto &pUpdateWeak : mp_RunningUpdates)
		{
			auto pUpdate = pUpdateWeak.f_Lock();
			if (pUpdate)
				pUpdate->m_bCancelOnAppManagerStop = true;
		}
		
		fp_OnAppUpdateInfoChange();
		
		return Continuation;
	}
}
