// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ConcurrencyManager>
#include <Mib/Concurrency/Actor/Timer>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCContinuation<CDistributedAppCommandLineResults> CAppManagerActor::fp_CommandLine_RemoveKnownHost(CEJSON const &_Params)
	{
		auto Auditor = f_Auditor();
		CStr Group = _Params["Group"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr HostID = _Params["HostID"].f_String();
		TCContinuation<CDistributedAppCommandLineResults> Continuation;
		
		TCActorResultVector<void> Results;
		
		for (auto &RemoteAppManager : mp_RemoteAppManagerState)
		{
			 if (!RemoteAppManager.m_Actor)
				 continue;
			DCallActor(RemoteAppManager.m_Actor, CAppManagerCoordinationInterface::f_RemoveKnownHost, Group, Application, HostID) > Results.f_AddResult();
		}
		
		mp_AppManagerCoordinationInterface.m_pActor->f_RemoveKnownHost(Group, Application, HostID) > Results.f_AddResult();
		
		Results.f_GetResults() > Continuation % Auditor / [Continuation](TCVector<TCAsyncResult<void>> &&_Results)
			{
				if (!fg_CombineResults(Continuation, fg_Move(_Results)))
					return;
				Continuation.f_SetResult();
			}
		;
		return Continuation;
	}

	TCContinuation<void> CAppManagerActor::fp_Coordination_WaitForAllToReachWantUpdateStage
		(
			TCSharedPointer<CApplication> const &_pApplication
			, CAppManagerInterface::EUpdateStage _Stage
			, fp64 _Timeout
			, bool _bIgnoreFailed
		)
	{
		CRemoteApplicationKey RemoteKey{_pApplication->m_Settings};
		
		return fp_Coordination_WaitForState
			(
				_pApplication
				, _Timeout
				, [=](TCContinuation<void> &o_Continuation) -> bool
				{
					bool bAllInStage = true;
					for (auto &RemoteAppManager : mp_RemoteAppManagerState)
					{
						for (auto &AppInfo : RemoteAppManager.m_AppInfos)
						{
							if (RemoteKey != AppInfo.m_AppInfo)
								continue;
							
							if (AppInfo.m_AppInfo.m_WantUpdateStage < _Stage)
								bAllInStage = false;
						}
					}
					
					// We might have several local applications with the same version manager application, so check locals as well 
					for (auto &pApplication : mp_Applications)
					{
						auto &Application = *pApplication;
						
						if (RemoteKey != Application.m_Settings)
							continue;
							
						if (Application.m_WantUpdateStage < _Stage)
							bAllInStage = false;
					}
					if (bAllInStage)
					{
						o_Continuation.f_SetResult();
						return true;
					}
					return false;
				}
				, fg_Format("Remote app manager failed before reaching '{}' stage", fsp_UpdateStageToStr(_Stage))
				, fg_Format("Timed out waiting for other AppManagers to reach '{}' stage", fsp_UpdateStageToStr(_Stage))
				, fg_Format("Remote app manager disappeared while waiting for other AppManagers to reach '{}' stage", fsp_UpdateStageToStr(_Stage))
				, _bIgnoreFailed
			)
		;
	}
	
	TCContinuation<void> CAppManagerActor::fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(TCSharedPointer<CApplication> const &_pApplication)
	{
		TCContinuation<void> Continuation;

		CRemoteApplicationKey RemoteKey{_pApplication->m_Settings};
		
		// First wait for all applications to be fully ready for doing update without being dependent on version manager
		fp_Coordination_WaitForAllToReachWantUpdateStage(_pApplication, CAppManagerInterface::EUpdateStage_StopOldApp, 30.0*60.0) 
			> Continuation / [this, RemoteKey, _pApplication, Continuation]
			{
				fp_Coordination_WaitForState
					(
						_pApplication
						, 60.0*60.0
						, [=](TCContinuation<void> &o_Continuation) -> bool
						{
							CStr SmallestHostID = mp_State.m_HostID;
							for (auto &RemoteAppManager : mp_RemoteAppManagerState)
							{
								bool bAllFinished = true;
								for (auto &AppInfo : RemoteAppManager.m_AppInfos)
								{
									if (RemoteKey != AppInfo.m_AppInfo)
										continue;
									
									if (AppInfo.m_AppInfo.m_UpdateStage != CAppManagerInterface::EUpdateStage_Finished)
									{
										bAllFinished = false;
										break;
									} 
								}
								if (bAllFinished)
									continue;
								SmallestHostID = fg_Min(SmallestHostID, mp_RemoteAppManagerState.fs_GetKey(RemoteAppManager));
							}
							
							if (SmallestHostID == mp_State.m_HostID)
							{
								o_Continuation.f_SetResult();
								return true;
							}
							return false;
						}
						, "Remote app manager failed while waiting for our turn to update"
						, "Timed out waiting for our turn to update"
						, "Remote app manager disappeared while waiting for our turn to update"
					)
					> Continuation 
				;
			}
		;
		return Continuation;
	}
	
	ch8 const *CAppManagerActor::fsp_UpdateStageToStr(CAppManagerInterface::EUpdateStage _Stage)
	{
		switch (_Stage)
		{
		case CAppManagerInterface::EUpdateStage_Failed: return "failed";
		case CAppManagerInterface::EUpdateStage_None: return "none";
		case CAppManagerInterface::EUpdateStage_ChangeEncryption: return "change encryption";
		case CAppManagerInterface::EUpdateStage_DownloadVersion: return "download version";
		case CAppManagerInterface::EUpdateStage_Unpack: return "unpack";
		case CAppManagerInterface::EUpdateStage_StopOldApp: return "stop old app";
		case CAppManagerInterface::EUpdateStage_PreUpdateScript: return "pre update script";
		case CAppManagerInterface::EUpdateStage_UpdateApplicationFiles: return "update application files";
		case CAppManagerInterface::EUpdateStage_SaveApplicationState: return "save application state";
		case CAppManagerInterface::EUpdateStage_PostUpdateScript: return "post update script";
		case CAppManagerInterface::EUpdateStage_StartNewApp: return "start new app";
		case CAppManagerInterface::EUpdateStage_PostLaunch: return "post launch";
		case CAppManagerInterface::EUpdateStage_Finished: return "finished";
		default: DNeverGetHere; return "unknown";
		}
	}
	
	TCContinuation<void> CAppManagerActor::fp_Coordination_WaitForState
		(
			TCSharedPointer<CApplication> const &_pApplication
			, fp64 _Timeout
			, TCFunctionMutable<bool (TCContinuation<void> &_Continuation)> &&_fEvalState
			, CStr const &_RemoteFailError
			, CStr const &_TimeoutError
			, CStr const &_DisconnectedError
			, bool _bIgnoreFailed
		)
	{
		CRemoteApplicationKey RemoteKey{_pApplication->m_Settings};
		
		TCSet<CStr> RemoteKnownHosts;
		auto *pRemoteKnownHosts = mp_KnownRemoteApplications.f_FindEqual(RemoteKey);
		if (pRemoteKnownHosts)
			RemoteKnownHosts = *pRemoteKnownHosts;

		TCSet<CStr> ConnectedRemoteHosts;
		ConnectedRemoteHosts[mp_State.m_HostID];
		
		for (auto &RemoteAppManager : mp_RemoteAppManagerState)
		{
			auto *pKnownApplications = RemoteAppManager.m_KnownApplications.f_FindEqual(RemoteKey);
			if (!RemoteAppManager.m_bInitialStateReceived || !pKnownApplications)
				continue;
			if (*pKnownApplications != RemoteKnownHosts)
				return DMibErrorInstance(fg_Format("AppManagers don't agree on known hosts for application '{vs}' != '{vs}'", *pKnownApplications, RemoteKnownHosts));
			
			ConnectedRemoteHosts[mp_RemoteAppManagerState.fs_GetKey(RemoteAppManager)];
		}
		
		if (ConnectedRemoteHosts != RemoteKnownHosts)
			return DMibErrorInstance(fg_Format("Not all known remote AppManagers are connected and up to date '{vs}' != '{vs}'", ConnectedRemoteHosts, RemoteKnownHosts));
		
		TCSharedPointer<COnRemoteApplicationInfoChange, CSupportWeakTag> pOnRemoteChange = fg_Construct();
		
		mp_OnRemoteApplicationInfoChange[pOnRemoteChange];
		
		TCContinuation<void> Continuation;
		
		pOnRemoteChange->m_fOnChanged = [=, pOnRemoteChangeWeak = pOnRemoteChange.f_Weak()]() mutable
			{
				auto pOnRemoteChange = pOnRemoteChangeWeak.f_Lock();
				if (!pOnRemoteChange)
					return;
				
				bool bFailed = false;
				TCSet<CStr> ConnectedRemoteHosts;
				ConnectedRemoteHosts[mp_State.m_HostID];
				
				for (auto &RemoteAppManager : mp_RemoteAppManagerState)
				{
					ConnectedRemoteHosts[mp_RemoteAppManagerState.fs_GetKey(RemoteAppManager)];
					if (!_bIgnoreFailed)
					{
						for (auto &AppInfo : RemoteAppManager.m_AppInfos)
						{
							if (RemoteKey != AppInfo.m_AppInfo)
								continue;
							
							if (AppInfo.m_AppInfo.m_WantUpdateStage == CAppManagerInterface::EUpdateStage_Failed)
								bFailed = true;
						}
					}
				}
				
				if (!_bIgnoreFailed)
				{
					// We might have several local applications with the same version manager application, so check locals as well 
					for (auto &pApplication : mp_Applications)
					{
						auto &Application = *pApplication;
						if (RemoteKey != Application.m_Settings)
							continue;
							
						if (Application.m_WantUpdateStage == CAppManagerInterface::EUpdateStage_Failed)
							bFailed = true;
					}
				}
				
				if (bFailed)
				{
					Continuation.f_SetException(DErrorInstance(_RemoteFailError));
					mp_OnRemoteApplicationInfoChange.f_Remove(pOnRemoteChangeWeak);
					return;
				}
				
				if (ConnectedRemoteHosts != RemoteKnownHosts)
				{
					Continuation.f_SetException(DErrorInstance(_DisconnectedError));
					mp_OnRemoteApplicationInfoChange.f_Remove(pOnRemoteChangeWeak);
					return;
				}
				
				if (_fEvalState(Continuation))
				{
					mp_OnRemoteApplicationInfoChange.f_Remove(pOnRemoteChangeWeak);
					return;
				}
			}
		;
		
		fg_OneshotTimerAbortable
			(
				_Timeout
				, self
				, [=, pOnRemoteChangeWeak = pOnRemoteChange.f_Weak()]
				{
					if (!Continuation.f_IsSet())
						Continuation.f_SetException(DErrorInstance(_TimeoutError));
					mp_OnRemoteApplicationInfoChange.f_Remove(pOnRemoteChangeWeak);
				}
			) 
			> [this, pOnRemoteChangeWeak = pOnRemoteChange.f_Weak()](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				auto pOnRemoteChange = pOnRemoteChangeWeak.f_Lock();
				if (!pOnRemoteChange)
					return;
				pOnRemoteChange->m_TimerSubscription = fg_Move(*_Subscription);
			}
		;
		
		pOnRemoteChange->m_fOnChanged();
		
		return Continuation;
	}
}
