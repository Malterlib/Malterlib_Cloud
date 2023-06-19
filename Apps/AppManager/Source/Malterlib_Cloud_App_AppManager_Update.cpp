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
	
	NConcurrency::TCFuture<void> CAppManagerActor::CAppManagerInterfaceImplementation::f_Update(NStr::CStr const &_Name, CApplicationUpdate const &_Update)
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
				, fg_GetCallingHostInfo()
			)
		;
	}
	
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_CancelAllUpdates(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		for (auto &pUpdateWeak : mp_RunningUpdates)
		{
			auto pUpdate = pUpdateWeak.f_Lock();
			if (pUpdate)
				pUpdate->m_bCancel = true;
		}
		
		fp_OnAppUpdateInfoChange();
		
		co_return {};
	}
	
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_UpdateApplication(CEJSONSorted _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		CStr Name = _Params["Name"].f_String();
		
		CStr Package;
		CAppManagerInterface::CApplicationUpdate Update;
		Update.m_bBypassCoordination = true;
		
		if (auto *pValue = _Params.f_GetMember("Package"))
		{
			Package = _Params["Package"].f_String();
			if (Package.f_IsEmpty())
				co_return DMibErrorInstance("You have to specify a package");
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
					co_return DMibErrorInstance(fg_Format("Invalid version ID format: {}", ErrorStr));
				Update.m_Version = VersionID;
			}
		}
		
		auto Result = co_await fp_UpdateApplication
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
				, fg_GetCallingHostInfo()
			)
			.f_Wrap()
		;

		co_return _pCommandLine->f_AddAsyncResult(Result);
	}
	
	TCFuture<void> CAppManagerActor::fp_UpdateApplication
		(
			CStr _Name
			, CAppManagerInterface::CApplicationUpdate _Update
			, CVersionManager::CVersionInformation _VersionInfo
			, CStr _FromFileName
			, TCFunction<void (CStr const &_Info)> _fOnInfo
			, CCallingHostInfo _CallingHostInfo
			, bool _bCheckPermissions
		)
	{
		auto Auditor = f_Auditor();

		if (_bCheckPermissions)
		{
			NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> Permissions;

			Permissions["Command"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationUpdate"}};
			Permissions["App"] = {CPermissionQuery{"AppManager/AppAll", fg_Format("AppManager/App/{}", _Name)}.f_Description("Access application {} in AppManager"_f << _Name)};

			auto HasPermissions = co_await
				(
					mp_Permissions.f_HasPermissions("Update application in AppManager", Permissions, _CallingHostInfo)
					% "Permission denied updating application"
					% Auditor
				)
			;

			if (!HasPermissions["Command"])
				co_return Auditor.f_AccessDenied("(Application update, command)", Permissions["Command"]);

			if (!HasPermissions["App"])
				co_return Auditor.f_AccessDenied("(Application update, app name)", Permissions["App"]);
		}

		auto pOldApplication = mp_Applications.f_FindEqual(_Name);
		if (!pOldApplication)
			co_return Auditor.f_Exception(fg_Format("No such application '{}'", _Name));

		TCSharedPointer<CApplication> pApplication = *pOldApplication;

		if (pApplication->f_IsInProgress())
			co_return Auditor.f_Exception("Operation already in progress for application");

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
				co_return Auditor.f_Exception("Invalid version platform format");

			auto RequiredTags = pApplication->m_Settings.m_UpdateTags;
			auto AllowedBranches = pApplication->m_Settings.m_UpdateBranches;

			if (_Update.m_RequireTags)
			{
				for (auto &Tag : *_Update.m_RequireTags)
				{
					if (!CVersionManager::fs_IsValidTag(Tag))
						co_return Auditor.f_Exception(fg_Format("'{}' is not a valid tag", Tag));
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
					co_return Auditor.f_Exception(fg_Format("Invalid version: {}", *_Update.m_Version));
				VersionID.m_VersionID = *_Update.m_Version;
				VersionInfo = _VersionInfo;
			}
			else
			{
				CStr Error;
				bool bVersionsChanged = false;
				CVersionManager::CVersionIDAndPlatform NewestUnconditionalVersion;
				CVersionManager::CVersionInformation NewestUnconditionalVersionInfo;

				VersionID = fp_FindVersion
					(
						pApplication
						, RequiredTags
						, AllowedBranches
						, Platform
						, Error
						, EFindVersionFlag_RetryFailed
						, VersionInfo
						, NewestUnconditionalVersion
						, NewestUnconditionalVersionInfo
						, bVersionsChanged
					)
				;

				if (!VersionID.f_IsValid())
					co_return Auditor.f_Exception(Error);

				if (VersionID != pApplication->m_WantVersion || VersionInfo != pApplication->m_WantVersionInfo)
				{
					bVersionsChanged = true;
					pApplication->m_WantVersion = VersionID;
					pApplication->m_WantVersionInfo = VersionInfo;
				}

				if (bVersionsChanged)
				{
					pApplication->m_NewestUnconditionalVersion = NewestUnconditionalVersion;
					pApplication->m_NewestUnconditionalVersionInfo = NewestUnconditionalVersionInfo;

					fp_SendAppChange_AddedOrChanged(*pApplication);

					co_await fp_UpdateApplicationJSON(pApplication);
				}
			}
		}

		if (VersionID.f_IsValid() && !VersionInfo.m_Time.f_IsValid())
		{
			if (pApplication->m_LastInstalledVersionFinished == VersionID)
				co_return Auditor.f_Exception("This version is already installed");
		}

		TCSharedPointer<NTime::CClock> pClock = fg_Construct(true);

		TCSharedPointerSupportWeak<CUpdateApplicationState> pState = fg_Construct();
		pState->m_pApplication = pApplication;
		pState->m_fOnInfo = fg_Move(_fOnInfo);
		pState->m_UniqueUpdateID = fg_RandomID();
		pState->m_LastInstalledVersion = pApplication->m_LastInstalledVersion;
		pState->m_LastInstalledVersionInfo = pApplication->m_LastInstalledVersionInfo;
		pState->m_VersionID = VersionID;
		pState->m_VersionTime = VersionInfo.m_Time;
		pState->m_VersionRetrySequence = VersionInfo.m_RetrySequence;
		pState->m_RequiredTags = RequiredTags;
		pState->m_bDryRun = bDryRun;
		pState->m_bBypassCoordination = _Update.m_bBypassCoordination;
		pState->m_Auditor = Auditor;
		pState->m_bUpdateSettings = bUpdateSettings;
		pState->m_pClock = pClock;
		pState->m_StartUpdateTime = NTime::CTime::fs_NowUTC();
		if (!bDownloadVersion)
			pState->m_SourcePath = _FromFileName;
		pState->m_pInProgressScope = pApplication->f_SetInProgress();

		mp_RunningUpdates[pState];
		pState->m_pCleanupStateMap = g_OnScopeExitActor / [this, pStateWeak = pState.f_Weak()]
			{
				mp_RunningUpdates.f_Remove(pStateWeak);
				if (mp_RunningUpdates.f_IsEmpty() && !mp_CancelRunningUpdatesOnStopAppManagerPromise.f_IsSet())
					mp_CancelRunningUpdatesOnStopAppManagerPromise.f_SetResult();
			}
		;

		pState->m_fOnInfo(fg_Format("Starting update with type '{}'", fsp_UpdateTypeToStr(pApplication->f_GetUpdateType(pState->m_bBypassCoordination))));

		auto Result = co_await fp_UpdateApplicationRunProcess(pState).f_Wrap();

		if (Result)
		{
			Auditor.f_Info(fg_Format("Application '{}' updated successfully", pState->m_pApplication->m_Name));

			co_await fp_SyncNotifications(_Name);
			
			co_return {};
		}

		CStr Error = Result.f_GetExceptionStr();

		if (Error.f_Find("AppManager stopped") >= 0 && pState->m_pApplication->m_UpdateStage <= EUpdateStage::EUpdateStage_SyncStart)
			co_return Auditor.f_Exception("Reschedule update after next restart");
		else
			fp_OnUpdateEvent(pState, EUpdateStage::EUpdateStage_Failed, Error) > fg_DiscardResult();

		self / [=, bUnencrypted = pState->m_bUnencrypted]() -> TCFuture<void>
			{
				if (!pApplication->m_bDeleted && bUnencrypted)
				{
					auto ScriptResult = co_await fp_RunUpdateScript
						(
							pApplication
							, EUpdateScript_OnError
							, Error
							, VersionID
							, pState->m_pVersionInfo.f_Get()
							, pState->m_LastInstalledVersion
							, pState->m_LastInstalledVersionInfo
							, pClock->f_GetTime()
						)
						.f_Wrap()
					;
					if (!ScriptResult)
					{
						DMibLogWithCategory
							(
								Malterlib/Cloud/AppManager
								, Warning
								, "Error script failed: {}"
								, ScriptResult.f_GetExceptionStr()
							)
						;
					}
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

				co_return {};
			}
			> fg_DiscardResult()
		;

		co_return Auditor.f_Exception(Error);
	}

	TCFuture<void> CAppManagerActor::fp_CancelAllApplicationUpdatesOnStopAppManager()
	{
		TCPromise<void> Promise;

		if (mp_RunningUpdates.f_IsEmpty())
			return Promise <<= g_Void;

		bool bNeedCancel = false;
		
		for (auto &pUpdateWeak : mp_RunningUpdates)
		{
			auto pUpdate = pUpdateWeak.f_Lock();
			if (pUpdate)
			{
				pUpdate->m_bCancelOnAppManagerStop = true;
				bNeedCancel = true;
			}
		}

		if (bNeedCancel)
			mp_CancelRunningUpdatesOnStopAppManagerPromise = Promise;
		else
			Promise.f_SetResult();

		fp_OnAppUpdateInfoChange();
		
		return Promise.f_MoveFuture();
	}
}
