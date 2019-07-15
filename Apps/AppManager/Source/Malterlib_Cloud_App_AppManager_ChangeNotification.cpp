// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_SubscribeChangeNotifications
		(
		 	NConcurrency::TCActorFunctorWithID<NConcurrency::TCFuture<void> (TCVector<CChangeNotification> &&_Notifications, bool _bInitial)> &&_fOnNotification
		)
		-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		CStr SubscriptionID = fg_RandomID();
		auto CallingHostInfo = fg_GetCallingHostInfo();

		bool bHasPermission = co_await
			(
				pThis->mp_Permissions.f_HasPermission("Subscribe to change notifications", {"AppManager/CommandAll", "AppManager/Command/ApplicationSubscribeChanges"})
			 	% "Permission denied subscribing to change notifications" % Auditor
			)
		;
		if (!bHasPermission)
			co_return Auditor.f_AccessDenied("(Subscribe to change notifications)");

		auto &Subscription = pThis->mp_ChangeNotificationSubscriptions[SubscriptionID];
		Subscription.m_fOnChange = fg_Move(_fOnNotification);
		Subscription.m_CallingHostInfo = CallingHostInfo;

		pThis->fp_SendInitialChangeNotifications(Subscription);

		Auditor.f_Info(fg_Format("Subscribe to change notifications '{}'", SubscriptionID));

		co_return
			(
				g_ActorSubscription / [pThis, SubscriptionID, Auditor]() -> TCFuture<void>
				{
					Auditor.f_Info(fg_Format("Unsubscribe from change notifications '{}'", SubscriptionID));

					auto pChangeNotificationSubscriptions = pThis->mp_ChangeNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!pChangeNotificationSubscriptions)
						co_return {};

					TCFuture<void> DestroyFuture = pChangeNotificationSubscriptions->m_fOnChange.f_Destroy();

					pThis->mp_ChangeNotificationSubscriptions.f_Remove(SubscriptionID);

					co_await fg_Move(DestroyFuture);
					co_return {};
				}
			)
		;
	}

	void CAppManagerActor::fp_SendInitialChangeNotifications(CChangeNotificationSubscription const &_Subscription)
	{
		DRequire(_Subscription.m_fOnChange);
		TCVector<CAppManagerInterface::CChangeNotification> Changes;

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;

			auto &ChangeNotification = Changes.f_Insert();
			ChangeNotification.m_Application = Application.m_Name;
			ChangeNotification.m_Change = CAppManagerInterface::CApplicationChange_AddOrChangeInfo{fp_GetApplicationInfo(Application)};
		}

		_Subscription.m_fOnChange(fg_Move(Changes), true) > fg_DiscardResult();
	}

	void CAppManagerActor::fp_SendAppChange_Status(CApplication const &_Application)
	{
		fp_SendChangeNotifications
			(
			 	CAppManagerInterface::CChangeNotification
			 	{
					_Application.m_Name
					, CAppManagerInterface::CApplicationChange_Status{_Application.m_LaunchStatus, _Application.m_LaunchStatusSeverity}
				}
			)
			> fg_DiscardResult()
		;
	}

	void CAppManagerActor::fp_SendAppChange_AddedOrChanged(CApplication const &_Application)
	{
		fp_SendChangeNotifications
			(
			 	CAppManagerInterface::CChangeNotification
			 	{
					_Application.m_Name
					, CAppManagerInterface::CApplicationChange_AddOrChangeInfo{fp_GetApplicationInfo(_Application)}
				}
			)
			> fg_DiscardResult()
		;
	}

	void CAppManagerActor::fp_SendAppChange_Removed(CApplication const &_Application)
	{
		fp_SendChangeNotifications
			(
			 	CAppManagerInterface::CChangeNotification
			 	{
					_Application.m_Name
					, CAppManagerInterface::CApplicationChange_Remove{}
				}
			)
			> fg_DiscardResult()
		;
	}

	TCFuture<void> CAppManagerActor::fp_SendChangeNotifications(CAppManagerInterface::CChangeNotification _Notification)
	{
		TCActorResultVector<void> OnChangeResultsVector;

		NContainer::TCVector<CAppManagerInterface::CChangeNotification> Notifications{_Notification};

		CStr AppPermission = fg_Format("AppManager/App/{}", _Notification.m_Application);

		for (auto &Subscription : mp_ChangeNotificationSubscriptions)
		{
			TCPromise<void> OnChangePromise;
			mp_Permissions.f_HasPermission("AppManager Change Event", {"AppManager/AppAll", AppPermission}, Subscription.m_CallingHostInfo)
				.f_Timeout(60.0, "Timed out waiting for permission in OnChange callback to {}"_f << Subscription.m_CallingHostInfo.f_GetRealHostID())
				> OnChangePromise / [=, SubscriptionID = mp_ChangeNotificationSubscriptions.fs_GetKey(Subscription)](bool _bHasPermission) mutable
				{
					auto pSubscription = mp_ChangeNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!_bHasPermission || !pSubscription)
						return OnChangePromise.f_SetResult();

					pSubscription->m_fOnChange
						(
						 	fg_Move(Notifications)
						 	, false
						)
						.f_Timeout(60.0, "Timed out waiting for OnChange callback to {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
						> OnChangePromise
					;
				}
			;
			OnChangePromise.f_MoveFuture() > OnChangeResultsVector.f_AddResult();
		}

		try
		{
			co_await OnChangeResultsVector.f_GetResults();
		}
		catch (CException const &_Exception)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Failed to send change notification: {}"
					, _Exception
				)
			;
		}

		co_return {};
	}
}
