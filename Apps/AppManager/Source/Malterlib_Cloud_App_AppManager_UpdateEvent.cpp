// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"
#include "Malterlib_Cloud_App_AppManager_Database.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	using namespace NAppManagerDatabase;

	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_SubscribeUpdateNotifications(CSubscribeUpdateNotifications _Params)
		-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		auto CallingHostInfo = fg_GetCallingHostInfo();

		NContainer::TCVector<NStr::CStr> Permissions = {"AppManager/CommandAll", "AppManager/Command/ApplicationSubscribeUpdates"};

		bool bHasPermission = co_await
			(
				pThis->mp_Permissions.f_HasPermission("Subscribe to update notifications", Permissions)
				% "Permission denied subscribing to update notifications" % Auditor
			)
		;
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Subscribe to update notifications)", Permissions);

		CStr SubscriptionID = fg_RandomID(pThis->mp_UpdateNotificationSubscriptions);
		auto &Subscription = pThis->mp_UpdateNotificationSubscriptions[SubscriptionID];
		Subscription.m_fOnUpdate = fg_Move(_Params.m_fOnNotification);
		Subscription.m_bWaitForResult = _Params.m_bWaitForNotification;
		Subscription.m_CallingHostInfo = CallingHostInfo;

		Auditor.f_Info(fg_Format("Subscribe to update notifications '{}'", SubscriptionID));

		if (_Params.m_LastSeenUniqueSequence != TCLimitsInt<uint64>::mc_Max)
		{
			pThis->fp_SendMissedUpdateNotifications(SubscriptionID, _Params.m_LastSeenUniqueSequence) > [pThis, SubscriptionID](TCAsyncResult<void> &&_Result)
				{
					if (!_Result)
						_Result > fg_LogError("UpdateNotifications", "Send missed update notification failed");

					auto pSubscription = pThis->mp_UpdateNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (pSubscription)
						pSubscription->m_bInitialFinished = true;
				}
			;
		}
		else
			Subscription.m_bInitialFinished = true;

		co_return
			(
				g_ActorSubscription / [pThis, SubscriptionID, Auditor]() -> TCFuture<void>
				{
					Auditor.f_Info(fg_Format("Unsubscribe from update notifications '{}'", SubscriptionID));

					auto pUpdateNotificationSubscriptions = pThis->mp_UpdateNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!pUpdateNotificationSubscriptions)
						co_return {};

					TCFutureVector<void> Destroys;
					fg_Move(pUpdateNotificationSubscriptions->m_fOnUpdate).f_Destroy() > Destroys;
					fg_Move(pUpdateNotificationSubscriptions->m_Sequencer).f_Destroy() > Destroys;

					pThis->mp_UpdateNotificationSubscriptions.f_Remove(SubscriptionID);

					co_await fg_AllDone(Destroys).f_Wrap() > fg_LogError("UpdateNotifications", "Failed to destroy subscription");

					co_return {};
				}
			)
		;
	}

	TCFuture<void> CAppManagerActor::fp_SendMissedUpdateNotifications(CStr _SubscriptionID, uint64 _LastSeenNotification)
	{
		CUpdateNotificationSubscription *pSubscription = nullptr;
		auto OnResume = co_await fg_OnResume
			(
				[&]() -> CExceptionPointer
				{
					pSubscription = mp_UpdateNotificationSubscriptions.f_FindEqual(_SubscriptionID);
					if (!pSubscription)
						return DMibErrorInstance("Subscription removed");
					return {};
				}
			)
		;

		auto SequenceSubscription = co_await pSubscription->m_Sequencer.f_Sequence();

		{
			auto ReadTransaction = co_await mp_DatabaseActor(&CDatabaseActor::f_OpenTransactionRead);

			auto NotificationsToSend = co_await fg_Move(ReadTransaction).f_BlockingDispatch
				(
					[_LastSeenNotification](CDatabaseActor::CTransactionRead &&_ReadTransaction)
					{
						auto iNotification = _ReadTransaction.m_Transaction.f_ReadCursor(CUpdateNotificationKey::mc_Prefix);
						iNotification.f_FindLowerBound(CUpdateNotificationKey::mc_Prefix, _LastSeenNotification);

						TCVector<CAppManagerInterface::CUpdateNotification> Return;
						for (; iNotification; ++iNotification)
						{
							auto Key = iNotification.f_Key<CUpdateNotificationKey>();
							if (Key.m_UniqueSequence <= _LastSeenNotification)
								continue;

							Return.f_Insert(fg_Move(iNotification.f_Value<CUpdateNotificationValue>().m_Notification));
						}

						return Return;
					}
					, "Error reading notification data from database"
				)
			;

			for (auto &Notification : NotificationsToSend)
				co_await fp_SendUpdateNotification(_SubscriptionID, Notification, false);
		}

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_SendUpdateNotification(CStr _SubscriptionID, CAppManagerInterface::CUpdateNotification _Notification, bool _bSequence)
	{
		TCFuture<void> OnUpdateFuture;
		{
			CUpdateNotificationSubscription *pSubscription = nullptr;

			auto OnResume = co_await fg_OnResume
				(
					[&]() -> CExceptionPointer
					{
						pSubscription = mp_UpdateNotificationSubscriptions.f_FindEqual(_SubscriptionID);
						if (!pSubscription)
							return DMibErrorInstance("Subscription removed");
						return {};
					}
				)
			;

			CActorSubscription SequenceSubscription;
			if (_bSequence && !pSubscription->m_bInitialFinished)
				SequenceSubscription = co_await pSubscription->m_Sequencer.f_Sequence();

			CStr AppPermission = fg_Format("AppManager/App/{}", _Notification.m_Application);

			bool bHasPermission = co_await mp_Permissions.f_HasPermission("AppManager Update Event", {"AppManager/AppAll", AppPermission}, pSubscription->m_CallingHostInfo)
				.f_Timeout(60.0, "Timed out waiting for permission in OnUpdate callback to {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
			;

			if (!bHasPermission)
				co_return {};

			OnUpdateFuture = pSubscription->m_fOnUpdate(fg_Move(_Notification))
				.f_Timeout(60.0, "Timed out waiting for OnUpdate callback to {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
			;
		}

		co_await fg_Move(OnUpdateFuture);

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_SendUpdateNotifications(CAppManagerInterface::CUpdateNotification _Notification)
	{
		_Notification.m_UniqueSequence = co_await fp_StoreUpdateNotification(_Notification);

		TCFutureVector<void> OnUpdateResultsVector;

		CStr AppPermission = fg_Format("AppManager/App/{}", _Notification.m_Application);

		for (auto &Subscription : mp_UpdateNotificationSubscriptions)
		{
			auto Future = fp_SendUpdateNotification(mp_UpdateNotificationSubscriptions.fs_GetKey(Subscription), _Notification, true);
			if (Subscription.m_bWaitForResult)
				fg_Move(Future) > OnUpdateResultsVector;
			else
				fg_Move(Future) > fg_LogWarning("UpdateNotifications", "Send non-waiting update notification failed");
		}

		auto Result = co_await fg_AllDone(OnUpdateResultsVector).f_Wrap();

		if (!Result)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Failed to send update notification: {}"
					, Result.f_GetExceptionStr()
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
		Notification.m_UpdateTime = _pState->m_pClock->f_GetTime();
		Notification.m_StartUpdateTime = _pState->m_StartUpdateTime;
		Notification.m_UpdateID = _pState->m_UniqueUpdateID;
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
				co_await fp_SendUpdateNotifications(Notification);
				co_await fp_Coordination_WaitForOurAppsTurnToUpdate(_pState);
			}

			auto UpdateType = Application.f_GetUpdateType(_pState->m_bBypassCoordination);
			switch (UpdateType)
			{
			case EDistributedAppUpdateType_AllAtOnce:
			case EDistributedAppUpdateType_OneAtATime:
				{
					if (_Stage == EUpdateStage::EUpdateStage_None)
					{
						co_await fp_SendUpdateNotifications(Notification);
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
						co_await fp_SendUpdateNotifications(Notification);
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
		co_await fp_SendUpdateNotifications(Notification);

		if (_Stage != Application.m_UpdateStage)
		{
			if (_Stage == EUpdateStage::EUpdateStage_Finished)
			{
				Application.m_LastInstalledVersionFinished = Application.m_LastInstalledVersion;
				Application.m_LastInstalledVersionInfoFinished = Application.m_LastInstalledVersionInfo;
				bUpdatedAppInfo = true;

				fp_SendAppChange_AddedOrChanged(*_pState->m_pApplication);
			}
			else if (_Stage == EUpdateStage::EUpdateStage_Failed)
			{
				bUpdatedAppInfo = true;
				if (Application.m_LastFailedInstalledVersionFailureStage != Application.m_UpdateStage)
				{
					Application.m_LastFailedInstalledVersionFailureStage = Application.m_UpdateStage;
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
				}
				Application.m_LastTriedInstalledVersionError = _Message;

				fp_SendAppChange_AddedOrChanged(*_pState->m_pApplication);
			}
			Application.m_UpdateStage = _Stage;
			fp_OnAppUpdateInfoChange(_pState->m_pApplication);
		}

		if (bUpdatedAppInfo)
			co_await fp_UpdateApplicationJson(_pState->m_pApplication);

		co_return {};
	}
}
