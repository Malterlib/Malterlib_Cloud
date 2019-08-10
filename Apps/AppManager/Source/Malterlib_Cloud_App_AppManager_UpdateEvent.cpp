// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_SubscribeUpdateNotifications
		(
			NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (CUpdateNotification const &_Notification)> &&_fOnNotification
		) 
		-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		CStr SubscriptionID = fg_RandomID();
		auto CallingHostInfo = fg_GetCallingHostInfo();

		bool bHasPermission = co_await
			(
				pThis->mp_Permissions.f_HasPermission("Subscribe to update notifications", {"AppManager/CommandAll", "AppManager/Command/ApplicationSubscribeUpdates"})
			 	% "Permission denied subscribing to update notifications" % Auditor
			)
		;
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Subscribe to update notifications)");

		auto &Subscription = pThis->mp_UpdateNotificationSubscriptions[SubscriptionID];
		Subscription.m_fOnUpdate = fg_Move(_fOnNotification);
		Subscription.m_CallingHostInfo = CallingHostInfo;

		Auditor.f_Info(fg_Format("Subscribe to update notifications '{}'", SubscriptionID));

		co_return
			(
				g_ActorSubscription / [pThis, SubscriptionID, Auditor]() -> TCFuture<void>
				{
					Auditor.f_Info(fg_Format("Unsubscribe from update notifications '{}'", SubscriptionID));

					auto pUpdateNotificationSubscriptions = pThis->mp_UpdateNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!pUpdateNotificationSubscriptions)
						co_return {};

					TCFuture<void> DestroyFuture = pUpdateNotificationSubscriptions->m_fOnUpdate.f_Destroy();

					pThis->mp_UpdateNotificationSubscriptions.f_Remove(SubscriptionID);

					co_await fg_Move(DestroyFuture);
					co_return {};
				}
			)
		;
	}

	TCFuture<void> CAppManagerActor::fp_SendUpdateNotifications
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
			, EUpdateStage _Stage
			, NStr::CStr _Message
			, CAppManagerInterface::CUpdateNotification _Notification
		 )
	{
		TCActorResultVector<void> OnUpdateResultsVector;

		CStr AppPermission = fg_Format("AppManager/App/{}", _Notification.m_Application);

		for (auto &Subscription : mp_UpdateNotificationSubscriptions)
		{
			TCPromise<void> OnUpdatePromise;
			mp_Permissions.f_HasPermission("AppManager Update Event", {"AppManager/AppAll", AppPermission}, Subscription.m_CallingHostInfo)
				.f_Timeout(60.0, "Timed out waiting for permission in OnUpdate callback to {}"_f << Subscription.m_CallingHostInfo.f_GetRealHostID())
				> OnUpdatePromise / [=, SubscriptionID = mp_UpdateNotificationSubscriptions.fs_GetKey(Subscription)](bool _bHasPermission)
				{
					auto pSubscription = mp_UpdateNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!_bHasPermission || !pSubscription)
						return OnUpdatePromise.f_SetResult();

					pSubscription->m_fOnUpdate(_Notification).f_Timeout(60.0, "Timed out waiting for OnUpdate callback to {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
						> OnUpdatePromise
					;
				}
			;
			OnUpdatePromise.f_MoveFuture() > OnUpdateResultsVector.f_AddResult();
		}

		try
		{
			co_await OnUpdateResultsVector.f_GetResults();
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Failed to send update notification: {}"
					, _Exception
				)
			;
		}

		co_return {};
	}


	TCFuture<void> CAppManagerActor::fp_OnUpdateEvent
		(
			TCSharedPointerSupportWeak<CUpdateApplicationState> _pState
			, EUpdateStage _Stage
			, NStr::CStr _Message
		)
	{
		auto &Application = *_pState->m_pApplication;

		CAppManagerInterface::CUpdateNotification Notification;
		Notification.m_Application = Application.m_Name;
		Notification.m_Message = _Message;
		Notification.m_VersionID = _pState->m_VersionID;
		Notification.m_VersionTime = _pState->m_VersionTime;
		Notification.m_Stage = _Stage;
		Notification.m_bCoordinateWait = true;

		{
			auto &Application = *_pState->m_pApplication;

			if (_Stage != Application.m_WantUpdateStage)
			{
				Application.m_WantUpdateStage = _Stage;
				if (_Stage == EUpdateStage::EUpdateStage_SyncStart)
					Application.m_UpdateStartSequence = ++mp_AppStageChangeSequence;
				fp_OnAppUpdateInfoChange(_pState->m_pApplication);
			}

			if (_Stage == EUpdateStage::EUpdateStage_SyncStart)
			{
				co_await fp_SendUpdateNotifications(_pState, _Stage, _Message, Notification);
				co_await fp_Coordination_WaitForOurAppsTurnToUpdate(_pState);
			}

			auto UpdateType = Application.f_GetUpdateType();
			switch (UpdateType)
			{
			case EDistributedAppUpdateType_AllAtOnce:
			case EDistributedAppUpdateType_OneAtATime:
				{
					if (_Stage == EUpdateStage::EUpdateStage_None)
					{
						co_await fp_SendUpdateNotifications(_pState, _Stage, _Message, Notification);
						co_await fp_Coordination_WaitForAllToReachWantUpdateStage
							(
								_pState
								, EUpdateStage::EUpdateStage_None
								, 10.0*60.0
								, EWaitStageFlag_IgnoreFailed | EWaitStageFlag_DisallowInProgressStates
								, TCFunctionMutable<bool ()>{}
							)
						;
					}
					else if (_Stage == EUpdateStage::EUpdateStage_StopOldApp)
					{
						co_await fp_SendUpdateNotifications(_pState, _Stage, _Message, Notification);
						if (UpdateType == EDistributedAppUpdateType_OneAtATime)
							co_await fp_Coordination_OneAtATime_WaitForOurTurnToUpdate(_pState);
						else
						{
							co_await fp_Coordination_WaitForAllToReachWantUpdateStage
								(
									_pState
									, EUpdateStage::EUpdateStage_StopOldApp
									, 30.0*60.0
									, EWaitStageFlag_None
									, TCFunctionMutable<bool ()>{}
								)
							;
						}
					}
					break;
				}
			case EDistributedAppUpdateType_Independent:
				{
					break;
				}
			default:
				{
					DNeverGetHere;
					break;
				}
			}
		}

		bool bUpdatedAppInfo = false;

		Notification.m_bCoordinateWait = false;
		co_await fp_SendUpdateNotifications(_pState, _Stage, _Message, Notification);

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
				if (Application.m_LastFailedInstalledVersionFailureStage != Application.m_UpdateStage)
				{
					Application.m_LastFailedInstalledVersionFailureStage = Application.m_UpdateStage;
					bUpdatedAppInfo = true;
				}
				if (Application.m_UpdateStage > EUpdateStage::EUpdateStage_DownloadVersion)
				{
					Application.m_LastFailedInstalledVersion = _pState->m_VersionID;
					Application.m_LastFailedInstalledVersionTime = _pState->m_VersionTime;
					Application.m_LastFailedInstalledVersionRetrySequence = _pState->m_VersionRetrySequence;
				}
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

		if (bUpdatedAppInfo)
			co_await fp_UpdateApplicationJSON(_pState->m_pApplication);

		co_return {};
	}
}
