// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/ConcurrencyManager>

#include "Malterlib_Cloud_App_AppManager.h"

namespace NMib::NCloud::NAppManager
{
	TCFuture<uint32> CAppManagerActor::fp_CommandLine_RemoveKnownHost(CEJSON _Params, NStorage::TCSharedPointer<CCommandLineControl> _pCommandLine)
	{
		auto Auditor = f_Auditor();
		CStr Group = _Params["Group"].f_String();
		CStr Application = _Params["Application"].f_String();
		CStr HostID = _Params["HostID"].f_String();

		TCActorResultVector<void> ResultsVector;

		for (auto &RemoteAppManager : mp_RemoteAppManagerState)
		{
			 if (!RemoteAppManager.m_Actor)
				 continue;
			RemoteAppManager.m_Actor.f_CallActor(&CAppManagerCoordinationInterface::f_RemoveKnownHost)(Group, Application, HostID) > ResultsVector.f_AddResult();
		}

		mp_AppManagerCoordinationInterface.m_pActor->f_RemoveKnownHost(Group, Application, HostID) > ResultsVector.f_AddResult();

		co_await (ResultsVector.f_GetResults() % Auditor) | (g_Unwrap % Auditor);

		co_return 0;
	}

	static bool fg_IsSameVersion
		(
			CVersionManager::CVersionIDAndPlatform const &_Left
			, CTime const &_LeftTime
			, CVersionManager::CVersionIDAndPlatform const &_Right
			, CTime const &_RightTime
		)
	{
		if (_Left.m_VersionID != _Right.m_VersionID)
			return false;
		if (!_LeftTime.f_IsValid() || !_RightTime.f_IsValid())
			return true;
		return _LeftTime == _RightTime;
	}

	struct CAppManagerActor::CFirstApplicationUpdate
	{
		CRemoteApplicationWithTypeKey m_RemoteKey;
		uint64 m_UpdateStartSequence = TCLimitsInt<uint64>::mc_Max;
		bool m_bInProgress = false;

		void f_AddApplication(EUpdateStage _WantUpdateState, uint64 _UpdateStartSequence, CRemoteApplicationWithTypeKey const &_RemoteKey)
		{
			if
				(
					_WantUpdateState >= EUpdateStage::EUpdateStage_SyncStart
					&& _WantUpdateState != EUpdateStage::EUpdateStage_Failed
					&& _WantUpdateState != EUpdateStage::EUpdateStage_Finished
				)
			{
				bool bInProgress = _WantUpdateState > EUpdateStage::EUpdateStage_SyncStart;
				if ((!m_bInProgress && _UpdateStartSequence < m_UpdateStartSequence) || bInProgress)
				{
					m_bInProgress = bInProgress;
					m_RemoteKey = _RemoteKey;
					m_UpdateStartSequence = _UpdateStartSequence;
				}
			}
		}

		bool f_IsValid() const
		{
			return m_UpdateStartSequence != TCLimitsInt<uint64>::mc_Max;
		}
	};

#define DDebugAppTurnUpdateLogic 0

	TCFuture<void> CAppManagerActor::fp_Coordination_WaitForOurAppsTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		CRemoteApplicationWithTypeKey OurRemoteKey{*_pState->m_pApplication, _pState->m_bBypassCoordination};

		_pState->m_fOnInfo(fg_Format("Waiting for our apps '{}' turn to update", OurRemoteKey));

		co_await fp_Coordination_WaitForAllToReachWantUpdateStage
			(
				_pState
				, EUpdateStage::EUpdateStage_SyncStart
				, 60.0*60.0
				, EWaitStageFlag_None
				, [=]() -> bool
				{
					TCMap<CRemoteApplicationWithTypeKey, zmint> WinningApplications;
					TCSet<CRemoteApplicationWithTypeKey> InProgressApplications;
#if DDebugAppTurnUpdateLogic > 1
					TCVector<CStr> ApplicationInfos;
#endif
					{
						CFirstApplicationUpdate FirstApplication;
						for (auto &pApplication : mp_Applications)
						{
							auto &Application = *pApplication;
							FirstApplication.f_AddApplication
								(
									Application.m_WantUpdateStage
									, Application.m_UpdateStartSequence
									, CRemoteApplicationWithTypeKey(Application, _pState->m_bBypassCoordination && pApplication == _pState->m_pApplication)
								)
							;
#if DDebugAppTurnUpdateLogic > 1
							ApplicationInfos.f_Insert
								(
									fg_Format
									(
										"{}:{}:{}:{}-{}"
										, mp_State.m_HostID
										, fsp_UpdateStageToStr(Application.m_WantUpdateStage)
										, Application.m_UpdateStartSequence
										, CRemoteApplicationWithTypeKey(Application, _pState->m_bBypassCoordination && pApplication == _pState->m_pApplication)
										, Application.m_Name
									)
								)
							;
#endif
						}

						if (FirstApplication.f_IsValid())
						{
							++WinningApplications[FirstApplication.m_RemoteKey];
							if (FirstApplication.m_bInProgress)
								InProgressApplications[FirstApplication.m_RemoteKey];
						}
					}

					for (auto &RemoteAppManager : mp_RemoteAppManagerState)
					{
						CFirstApplicationUpdate FirstApplication;
						for (auto &AppInfo : RemoteAppManager.m_AppInfos)
						{
							FirstApplication.f_AddApplication
								(
									AppInfo.m_AppInfo.m_WantUpdateStage
									, AppInfo.m_AppInfo.m_UpdateStartSequence
									, CRemoteApplicationWithTypeKey(AppInfo.m_AppInfo, false)
								)
							;
#if DDebugAppTurnUpdateLogic > 1
							ApplicationInfos.f_Insert
								(
									fg_Format
									(
										"{}:{}:{}:{}-{}"
										, RemoteAppManager.m_AppInfos.fs_GetKey(AppInfo)
										, fsp_UpdateStageToStr(AppInfo.m_AppInfo.m_WantUpdateStage)
										, AppInfo.m_AppInfo.m_UpdateStartSequence
										, CRemoteApplicationWithTypeKey(AppInfo.m_AppInfo, false)
										, RemoteAppManager.m_AppInfos.fs_GetKey(AppInfo)
									)
								)
							;
#endif
						}

						if (FirstApplication.f_IsValid())
						{
							++WinningApplications[FirstApplication.m_RemoteKey];
							if (FirstApplication.m_bInProgress)
								InProgressApplications[FirstApplication.m_RemoteKey];
						}
					}

					if (!InProgressApplications.f_IsEmpty())
					{
						for (auto iApplication = WinningApplications.f_GetIterator(); iApplication;)
						{
							if (!InProgressApplications.f_FindEqual(iApplication.f_GetKey()))
							{
								iApplication.f_Remove();
								continue;
							}
							++iApplication;
						}
					}

#if DDebugAppTurnUpdateLogic > 1
					DMibConOut2
						(
						 	"[{}, {a-,sj20}, {a-,sj8}] # {vs} - {vs} - {vs}\n"
							, this
							, OurRemoteKey
							, _pState->m_pApplication->m_Name
							, InProgressApplications
							, WinningApplications
							, ApplicationInfos
						)
					;
#endif
					auto *pWinning = WinningApplications.f_FindSmallest();
					if (!pWinning)
						return false;

					if (WinningApplications.fs_GetKey(*pWinning) == OurRemoteKey)
					{
#if DDebugAppTurnUpdateLogic > 0
						DMibConOut2("[{}, {a-,sj20}, {a-,sj8}] # UPDATE\n", this, OurRemoteKey, _pState->m_pApplication->m_Name);
#endif
						_pState->m_fOnInfo(fg_Format("Our apps '{}' turn to update", OurRemoteKey));
						return true;
					}
					return false;
				}
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_Coordination_WaitForAllToReachWantUpdateStage
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
			, EUpdateStage _Stage
			, fp64 _Timeout
			, EWaitStageFlag _Flags
			, TCFunctionMutable<bool ()> _fEvalState
		)
	{
		CRemoteApplicationWithTypeKey RemoteKey(*_pState->m_pApplication, _pState->m_bBypassCoordination);

		if (!_fEvalState)
			_pState->m_fOnInfo(fg_Format("Waiting for all before starting stage '{}'", fsp_UpdateStageToStr(_Stage)));

		co_await fp_Coordination_WaitForState
			(
				_pState
				, _Timeout
				, [=](TCPromise<void> &o_Promise) mutable -> bool
				{
					bool bAllInStage = true;

					if (RemoteKey.m_UpdateType != EDistributedAppUpdateType_Independent)
					{
						auto fCheckStageCondition = [&]
							(
								EUpdateStage _CurrentStage
								, CVersionManager::CVersionIDAndPlatform const &_CurrentVersionID
								, CTime const &_CurrentVersionTime
							)
							{
								if (fg_IsSameVersion(_CurrentVersionID, _CurrentVersionTime, _pState->m_VersionID, _pState->m_VersionTime))
									return true; // Already installed this version

								if (_Flags & EWaitStageFlag_DisallowInProgressStates)
								{
									if (_CurrentStage > EUpdateStage::EUpdateStage_SyncStart)
										return false;
								}
								if (_CurrentStage < _Stage)
									return false;
								return true;
							}
						;

						for (auto &RemoteAppManager : mp_RemoteAppManagerState)
						{
							for (auto &AppInfo : RemoteAppManager.m_AppInfos)
							{
								if (!AppInfo.m_AppInfo.m_bAutoUpdate)
									continue;
								if (RemoteKey != CRemoteApplicationWithTypeKey(AppInfo.m_AppInfo, false))
									continue;
								if (!fCheckStageCondition(AppInfo.m_AppInfo.m_WantUpdateStage, AppInfo.m_AppInfo.m_VersionID, AppInfo.m_AppInfo.m_VersionTime))
									bAllInStage = false;
							}
						}

						// We might have several local applications with the same version manager application, so check locals as well
						for (auto &pApplication : mp_Applications)
						{
							auto &Application = *pApplication;

							if (!Application.m_Settings.m_bAutoUpdate)
								continue;

							if (RemoteKey != CRemoteApplicationWithTypeKey(Application, _pState->m_bBypassCoordination && pApplication == _pState->m_pApplication))
								continue;
							if (!fCheckStageCondition(Application.m_WantUpdateStage, Application.m_LastInstalledVersionFinished, Application.m_LastInstalledVersionInfoFinished.m_Time))
								bAllInStage = false;
						}
					}

					if (bAllInStage)
					{
						if (_fEvalState && !_fEvalState())
							return false;
						if (!_fEvalState)
							_pState->m_fOnInfo(fg_Format("All ready to start stage '{}'", fsp_UpdateStageToStr(_Stage)));
						o_Promise.f_SetResult();
						return true;
					}
					return false;
				}
				, fg_Format("Remote app manager failed before reaching '{}' stage", fsp_UpdateStageToStr(_Stage))
				, fg_Format("Timed out waiting for other AppManagers to reach '{}' stage", fsp_UpdateStageToStr(_Stage))
				, fg_Format("Timed out waiting for remote app manager to connect while waiting for other AppManagers to reach '{}' stage", fsp_UpdateStageToStr(_Stage))
				, (_Flags & EWaitStageFlag_IgnoreFailed) != 0
			)
		;

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(TCSharedPointerSupportWeak<CUpdateApplicationState> _pState)
	{
		CRemoteApplicationWithTypeKey RemoteKey(*_pState->m_pApplication, _pState->m_bBypassCoordination);

		// First wait for all applications to be fully ready for doing update without being dependent on version manager
		co_await fp_Coordination_WaitForAllToReachWantUpdateStage
			(
				_pState
				, EUpdateStage::EUpdateStage_StopOldApp
				, 30.0*60.0
				, EWaitStageFlag_None
				, TCFunctionMutable<bool ()>{}
			)
		;

		_pState->m_fOnInfo("Waiting for our turn to update");

		co_await fp_Coordination_WaitForState
			(
				_pState
				, 60.0*60.0
				, [=](TCPromise<void> &o_Promise) -> bool
				{
					CStr SmallestHostID = mp_State.m_HostID;
					for (auto &RemoteAppManager : mp_RemoteAppManagerState)
					{
						bool bAllFinished = true;
						for (auto &AppInfo : RemoteAppManager.m_AppInfos)
						{
							if (!AppInfo.m_AppInfo.m_bAutoUpdate)
								continue;

							if (RemoteKey != CRemoteApplicationWithTypeKey(AppInfo.m_AppInfo, false))
								continue;

							if (fg_IsSameVersion(AppInfo.m_AppInfo.m_VersionID, AppInfo.m_AppInfo.m_VersionTime, _pState->m_VersionID, _pState->m_VersionTime))
								continue; // Already installed this version

							if (AppInfo.m_AppInfo.m_UpdateStage != EUpdateStage::EUpdateStage_Finished)
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
						CStr SmallestApplication;
						for (auto &pApplication : mp_Applications)
						{
							auto &Application = *pApplication;

							if (!Application.m_Settings.m_bAutoUpdate)
								continue;

							if (RemoteKey != CRemoteApplicationWithTypeKey(Application, _pState->m_bBypassCoordination && pApplication == _pState->m_pApplication))
								continue;

							if
								(
								 	fg_IsSameVersion
								 	(
									 	Application.m_LastInstalledVersionFinished
									 	, Application.m_LastInstalledVersionInfoFinished.m_Time
									 	, _pState->m_VersionID
									 	, _pState->m_VersionTime
									)
								)
							{
								continue; // Already installed this version
							}

							if (Application.m_UpdateStage == EUpdateStage::EUpdateStage_Finished)
								continue;

							if (SmallestApplication.f_IsEmpty() || Application.m_Name < SmallestApplication)
								SmallestApplication = Application.m_Name;
						}

						if (_pState->m_pApplication->m_Name == SmallestApplication)
						{
#if DDebugAppTurnUpdateLogic > 0
							DMibConOut2("[{}, {a-,sj20}, {a-,sj8}] # Smallest updating\n", this, RemoteKey, _pState->m_pApplication->m_Name);
#endif
							_pState->m_fOnInfo("Our turn to update");
							o_Promise.f_SetResult();
							return true;
						}
						else
						{
#if DDebugAppTurnUpdateLogic > 1
							DMibConOut2("[{}, {a-,sj20}, {a-,sj8}] # NOT Smallest {2} != {3}\n", this, RemoteKey, _pState->m_pApplication->m_Name, SmallestApplication);
#endif
						}
					}
					return false;
				}
				, "Remote app manager failed while waiting for our turn to update"
				, "Timed out waiting for our turn to update"
				, "Timed out waiting for remote app manager to connect while waiting for our turn to update"
				, false
			)
		;

		co_return {};
	}

	ch8 const *CAppManagerActor::fsp_UpdateStageToStr(EUpdateStage _Stage)
	{
		switch (_Stage)
		{
		case EUpdateStage::EUpdateStage_Failed: return "failed";
		case EUpdateStage::EUpdateStage_None: return "none";
		case EUpdateStage::EUpdateStage_SyncStart: return "sync start";
		case EUpdateStage::EUpdateStage_ChangeEncryption: return "change encryption";
		case EUpdateStage::EUpdateStage_DownloadVersion: return "download version";
		case EUpdateStage::EUpdateStage_Unpack: return "unpack";
		case EUpdateStage::EUpdateStage_StopOldApp: return "stop old app";
		case EUpdateStage::EUpdateStage_PreUpdateScript: return "pre update script";
		case EUpdateStage::EUpdateStage_UpdateApplicationFiles: return "update application files";
		case EUpdateStage::EUpdateStage_SaveApplicationState: return "save application state";
		case EUpdateStage::EUpdateStage_PostUpdateScript: return "post update script";
		case EUpdateStage::EUpdateStage_StartNewApp: return "start new app";
		case EUpdateStage::EUpdateStage_PostLaunch: return "post launch";
		case EUpdateStage::EUpdateStage_Finished: return "finished";
		default: DNeverGetHere; return "unknown";
		}
	}

	TCFuture<void> CAppManagerActor::fp_Coordination_WaitForState
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
			, fp64 _Timeout
			, TCFunctionMutable<bool (TCPromise<void> &_Promise)> _fEvalState
			, CStr _RemoteFailError
			, CStr _TimeoutError
			, CStr _DisconnectedError
			, bool _bIgnoreFailed
		)
	{
		TCPromise<void> Promise;

		CRemoteApplicationWithTypeKey RemoteKey(*_pState->m_pApplication, _pState->m_bBypassCoordination);

		TCSharedPointerSupportWeak<COnAppUpdateInfoChange> pOnAppUpdateInfoChange = fg_Construct();
		mp_OnAppUpdateInfoChange[pOnAppUpdateInfoChange];

		struct CState
		{
			CClock m_DisconnectClock;
			bool m_bWasDisconnected = false;
		};

		TCSharedPointer<CState> pState = fg_Construct();

		pOnAppUpdateInfoChange->m_fOnChanged = [=, pOnAppUpdateInfoChangeWeak = pOnAppUpdateInfoChange.f_Weak()]() mutable
			{
				auto pOnAppUpdateInfoChange = pOnAppUpdateInfoChangeWeak.f_Lock();
				if (!pOnAppUpdateInfoChange)
					return;

				if (_pState->f_CheckAbort(Promise))
				{
					mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
					return;
				}

				bool bFailed = false;
				TCSet<CStr> ConnectedRemoteHosts;
				ConnectedRemoteHosts[mp_State.m_HostID];

				TCSet<CStr> RemoteKnownHosts;
				auto *pRemoteKnownHosts = mp_KnownRemoteApplications.f_FindEqual(RemoteKey.f_WithoutType());
				if (pRemoteKnownHosts)
					RemoteKnownHosts = *pRemoteKnownHosts;

				for (auto &RemoteAppManager : mp_RemoteAppManagerState)
				{
					auto *pKnownApplications = RemoteAppManager.m_KnownApplications.f_FindEqual(RemoteKey.f_WithoutType());
					if (!RemoteAppManager.m_bInitialStateReceived || !pKnownApplications)
						continue;
					if (*pKnownApplications != RemoteKnownHosts)
					{
						if (!Promise.f_IsSet())
						{
							Promise.f_SetException
								(
									DMibErrorInstance(fg_Format("AppManagers don't agree on known hosts for application '{vs}' != '{vs}'", *pKnownApplications, RemoteKnownHosts))
								)
							;
						}
						mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
						return;
					}

					ConnectedRemoteHosts[mp_RemoteAppManagerState.fs_GetKey(RemoteAppManager)];

					for (auto &AppInfo : RemoteAppManager.m_AppInfos)
					{
						if (RemoteKey != CRemoteApplicationWithTypeKey(AppInfo.m_AppInfo, false))
							continue;

						if (fg_IsSameVersion(AppInfo.m_AppInfo.m_VersionID, AppInfo.m_AppInfo.m_VersionTime, _pState->m_VersionID, _pState->m_VersionTime))
							continue; // Already installed this version

						if
							(
								fg_IsSameVersion(AppInfo.m_AppInfo.m_FailedVersionID, AppInfo.m_AppInfo.m_FailedVersionTime, _pState->m_VersionID, _pState->m_VersionTime)
								&& AppInfo.m_AppInfo.m_FailedVersionRetrySequence == _pState->m_VersionRetrySequence
							)
						{
							bFailed = true;
							break;
						}

						if (AppInfo.m_AppInfo.m_WantUpdateStage == EUpdateStage::EUpdateStage_Failed && !_bIgnoreFailed)
						{
							bFailed = true;
							break;
						}
					}
				}

				// We might have several local applications with the same version manager application, so check locals as well
				for (auto &pApplication : mp_Applications)
				{
					auto &Application = *pApplication;
					if (RemoteKey != CRemoteApplicationWithTypeKey(Application, _pState->m_bBypassCoordination && pApplication == _pState->m_pApplication))
						continue;

					if (fg_IsSameVersion(Application.m_LastInstalledVersionFinished, Application.m_LastInstalledVersionInfoFinished.m_Time, _pState->m_VersionID, _pState->m_VersionTime))
						continue; // Already installed this version

					if
						(
							fg_IsSameVersion(Application.m_LastFailedInstalledVersion, Application.m_LastFailedInstalledVersionTime, _pState->m_VersionID, _pState->m_VersionTime)
							&& Application.m_LastFailedInstalledVersionRetrySequence == _pState->m_VersionRetrySequence
						)
					{
						bFailed = true;
						break;
					}

					if (Application.m_WantUpdateStage == EUpdateStage::EUpdateStage_Failed && !_bIgnoreFailed)
					{
						bFailed = true;
						break;
					}
				}

				if (bFailed)
				{
					if (!Promise.f_IsSet())
						Promise.f_SetException(DErrorInstance(_RemoteFailError));
					mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
					return;
				}

				if (ConnectedRemoteHosts != RemoteKnownHosts)
				{
					if (!pState->m_bWasDisconnected)
					{
						pState->m_bWasDisconnected = true;
						pState->m_DisconnectClock.f_Start();
					}
					return; // Wait for missing to connect
				}

				pState->m_bWasDisconnected = false;

				if (_fEvalState(Promise))
				{
					mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
					return;
				}
			}
		;

		fp64 DisconnectTimeout = 5.0*60.0;
		if (_Timeout <= DisconnectTimeout)
			DisconnectTimeout = _Timeout / 2.0;

		fg_RegisterTimer
			(
				1.0*60.0
				, [=, pOnAppUpdateInfoChangeWeak = pOnAppUpdateInfoChange.f_Weak()]() -> TCFuture<void>
				{
					if (pState->m_bWasDisconnected && pState->m_DisconnectClock.f_GetTime() >= DisconnectTimeout)
					{
						if (!Promise.f_IsSet())
							Promise.f_SetException(DErrorInstance(_DisconnectedError));
						mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
					}
					co_return {};
				}
			)
			> [pOnAppUpdateInfoChangeWeak = pOnAppUpdateInfoChange.f_Weak()](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				auto pOnAppUpdateInfoChange = pOnAppUpdateInfoChangeWeak.f_Lock();
				if (!pOnAppUpdateInfoChange)
					return;
				pOnAppUpdateInfoChange->m_DisconnectTimerSubscription = fg_Move(*_Subscription);
			}
		;

		fg_OneshotTimerAbortable
			(
				_Timeout
				, [=, pOnAppUpdateInfoChangeWeak = pOnAppUpdateInfoChange.f_Weak()]
				{
					if (!Promise.f_IsSet())
						Promise.f_SetException(DErrorInstance(_TimeoutError));
					mp_OnAppUpdateInfoChange.f_Remove(pOnAppUpdateInfoChangeWeak);
				}
			)
			> [pOnAppUpdateInfoChangeWeak = pOnAppUpdateInfoChange.f_Weak()](TCAsyncResult<CActorSubscription> &&_Subscription)
			{
				auto pOnAppUpdateInfoChange = pOnAppUpdateInfoChangeWeak.f_Lock();
				if (!pOnAppUpdateInfoChange)
					return;
				pOnAppUpdateInfoChange->m_TimerSubscription = fg_Move(*_Subscription);
			}
		;

		pOnAppUpdateInfoChange->m_fOnChanged();

		return Promise.f_MoveFuture();
	}
}
