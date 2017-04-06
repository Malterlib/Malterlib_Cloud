// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_SubscribeUpdateNotifications
		(
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCContinuation<void> (CUpdateNotification const &_Notification)> &&_fOnNotification
		) 
		-> NConcurrency::TCContinuation<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		CStr SubscriptionID = fg_RandomID();
		CStr CallingHostID = fg_GetCallingHostID();

		if (!pThis->mp_Permissions.f_HostHasAnyPermission(CallingHostID, "AppManager/CommandAll", "AppManager/Command/ApplicationSubscribeUpdates"))
			return Auditor.f_AccessDenied("(Application enum)");
		
		auto &Subscription = pThis->mp_UpdateNotificationSubscriptions[SubscriptionID];
		Subscription.m_fOnUpdate = fg_Move(_fOnNotification);
		Subscription.m_CallingHostID = CallingHostID;
		
		Auditor.f_Info(fg_Format("Subscribe to update notifications '{}'", SubscriptionID));

		return fg_Explicit
			(	
				g_ActorSubscription > [pThis, SubscriptionID, Auditor]() -> TCContinuation<void>
				{
					Auditor.f_Info(fg_Format("Unsubscribe from update notifications '{}'", SubscriptionID));
					
					auto pUpdateNotificationSubscriptions = pThis->mp_UpdateNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!pUpdateNotificationSubscriptions)
						return fg_Explicit();
					
					TCContinuation<void> Continuation = pUpdateNotificationSubscriptions->m_fOnUpdate.f_Destroy();
					
					pThis->mp_UpdateNotificationSubscriptions.f_Remove(SubscriptionID);
					
					return Continuation;
				}
			)
		;
	}

	TCContinuation<void> CAppManagerActor::fp_OnUpdateEvent
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> const &_pState
			, EUpdateStage _Stage
			, NStr::CStr const &_Message
		)
	{
		auto &Application = *_pState->m_pApplication;
				
		if (_Stage != Application.m_WantUpdateStage)
		{
			Application.m_WantUpdateStage = _Stage;
			if (_Stage == EUpdateStage::EUpdateStage_SyncStart)
				Application.m_UpdateStartSequence = ++mp_AppStageChangeSequence;
			fp_OnAppUpdateInfoChange(_pState->m_pApplication);
		}
		
		TCContinuation<void> CoordinationContinuation;

		auto UpdateType = Application.f_GetUpdateType(); 
		switch (UpdateType)
		{
		case EDistributedAppUpdateType_AllAtOnce:
		case EDistributedAppUpdateType_OneAtATime:
			{
				if (_Stage == EUpdateStage::EUpdateStage_None)
				{
					CoordinationContinuation = fp_Coordination_WaitForAllToReachWantUpdateStage
						(
							_pState
							, EUpdateStage::EUpdateStage_None
							, 10.0*60.0
							, EWaitStageFlag_IgnoreFailed | EWaitStageFlag_DisallowInProgressStates
						)
					;
				}
				else if (_Stage == EUpdateStage::EUpdateStage_SyncStart)
					CoordinationContinuation = fp_Coordination_WaitForOurAppsTurnToUpdate(_pState);
				else if (_Stage == EUpdateStage::EUpdateStage_StopOldApp)
				{
					if (UpdateType == EDistributedAppUpdateType_OneAtATime)
						CoordinationContinuation = fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(_pState);
					else
						CoordinationContinuation = fp_Coordination_WaitForAllToReachWantUpdateStage(_pState, EUpdateStage::EUpdateStage_StopOldApp, 30.0*60.0, EWaitStageFlag_None);
				}
				else
					CoordinationContinuation.f_SetResult();
				break;
			}
		case EDistributedAppUpdateType_Independent:
			{
				CoordinationContinuation.f_SetResult();
				break;
			}
		default:
			{
				DNeverGetHere;
				break;
			}
		}
		
		TCContinuation<void> Continuation;
		
		CoordinationContinuation.f_Dispatch() > Continuation / [=]
			{
				auto &Application = *_pState->m_pApplication;
		
				bool bUpdatedAppInfo = false;
				
				if (_Stage != Application.m_UpdateStage)
				{
					if (_Stage == EUpdateStage::EUpdateStage_Finished)
					{
						Application.m_LastInstalledVersionFinished = Application.m_LastInstalledVersion;
						Application.m_LastInstalledVersionInfoFinished = Application.m_LastInstalledVersionInfo;
						bUpdatedAppInfo = true;
					}
					else if (_Stage == EUpdateStage::EUpdateStage_Failed)
					{
						Application.m_LastFailedInstalledVersion = _pState->m_VersionID;
						Application.m_LastFailedInstalledVersionTime = _pState->m_VersionTime;
						Application.m_LastFailedInstalledVersionRetrySequence = _pState->m_VersionRetrySequence; 
						if (!_pState->m_bSetLastTried)
						{
							Application.m_LastTriedInstalledVersion = _pState->m_VersionID;
							Application.m_LastTriedInstalledVersionInfo.m_Time = _pState->m_VersionTime;
							Application.m_LastTriedInstalledVersionInfo.m_RetrySequence = _pState->m_VersionRetrySequence;
							bUpdatedAppInfo = true;
						}
					}
					Application.m_UpdateStage = _Stage;
					fp_OnAppUpdateInfoChange(_pState->m_pApplication);
				}
				
				CAppManagerInterface::CUpdateNotification Notification;
				Notification.m_Application = Application.m_Name;
				Notification.m_Message = _Message;
				Notification.m_VersionID = _pState->m_VersionID;
				Notification.m_VersionTime = _pState->m_VersionTime;
				Notification.m_Stage = _Stage;
				
				CStr AppPermission = fg_Format("AppManager/App/{}", Application.m_Name);
				
				for (auto &Subscription : mp_UpdateNotificationSubscriptions)
				{
					if (!mp_Permissions.f_HostHasAnyPermission(Subscription.m_CallingHostID, "AppManager/AppAll", AppPermission))
						continue;
					
					Subscription.m_fOnUpdate(Notification) > fg_DiscardResult();
				}
				
				if (!bUpdatedAppInfo)
				{
					Continuation.f_SetResult();
					return;
				}
				
				fp_UpdateApplicationJSON(_pState->m_pApplication) > Continuation;
			}
		;
		
		return Continuation;
	}
}
