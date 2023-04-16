// Copyright © 2015 Hansoft AB
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include "Malterlib_Cloud_App_AppManager.h"

#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>
#include <Mib/Concurrency/LogError>

namespace NMib::NCloud::NAppManager
{
	auto CAppManagerActor::CAppManagerInterfaceImplementation::f_SubscribeChangeNotifications(CSubscribeChangeNotifications &&_Params)
		-> NConcurrency::TCFuture<NConcurrency::TCActorSubscriptionWithID<>>
	{
		auto pThis = m_pThis;
		auto Auditor = pThis->f_Auditor();
		CStr SubscriptionID = fg_RandomID(pThis->mp_ChangeNotificationSubscriptions);
		auto CallingHostInfo = fg_GetCallingHostInfo();

		auto &Subscription = pThis->mp_ChangeNotificationSubscriptions[SubscriptionID];
		Subscription.m_fOnChange = fg_Move(_Params.m_fOnNotification);
		Subscription.m_bWaitForResult = _Params.m_bWaitForNotification;
		Subscription.m_CallingHostInfo = CallingHostInfo;

		Auditor.f_Info("Subscribe to change notifications '{}'"_f << SubscriptionID);

		auto ReturnSubscription = g_ActorSubscription / [pThis, SubscriptionID, Auditor]() -> TCFuture<void>
			{
				Auditor.f_Info(fg_Format("Unsubscribe from change notifications '{}'", SubscriptionID));

				auto pChangeNotificationSubscriptions = pThis->mp_ChangeNotificationSubscriptions.f_FindEqual(SubscriptionID);
				if (!pChangeNotificationSubscriptions)
					co_return {};

				TCFuture<void> DestroyFuture = fg_Move(pChangeNotificationSubscriptions->m_fOnChange).f_Destroy();

				pThis->mp_ChangeNotificationSubscriptions.f_Remove(SubscriptionID);

				co_await fg_Move(DestroyFuture);
				co_return {};
			}
		;

		co_await pThis->self(&CAppManagerActor::fp_ChangeNotifications_SendInitial, SubscriptionID);

		co_return fg_Move(ReturnSubscription);
	}

	TCFuture<void> CAppManagerActor::fp_ChangeNotifications_SendInitial(CStr const &_SubscriptionID)
	{
		NContainer::TCMap<NStr::CStr, NContainer::TCVector<CPermissionQuery>> NamedPermissionsQueries;
		auto pSubscription = mp_ChangeNotificationSubscriptions.f_FindEqual(_SubscriptionID);
		if (!pSubscription)
			co_return {};

		pSubscription->m_bInitialFinished = false;
		pSubscription->m_bAccessDenied = false;
		pSubscription->m_OnInitialFinished.f_Clear();

		NamedPermissionsQueries["/Command/"] = {{"AppManager/CommandAll", "AppManager/Command/ApplicationSubscribeChanges"}};

		for (auto &pApplication : mp_Applications)
		{
			auto &Application = *pApplication;
			NamedPermissionsQueries["/App/" + Application.m_Name] = {{"AppManager/AppAll", "AppManager/App/{}"_f << Application.m_Name}};
		}

		auto Permissions = co_await mp_Permissions.f_HasPermissions("Initial send change notifications", fg_Move(NamedPermissionsQueries), pSubscription->m_CallingHostInfo)
			.f_Timeout(60.0, "Timed out waiting for permission in send initial change notifications {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
		;

		pSubscription = mp_ChangeNotificationSubscriptions.f_FindEqual(_SubscriptionID);
		if (!pSubscription)
			co_return {};

		DRequire(pSubscription->m_fOnChange);

		CAppManagerInterface::COnChangeNotificationParams NotificationParams;

		auto pCommandPermission = Permissions.f_FindEqual("/Command/");
		bool bHasPermission = pCommandPermission && *pCommandPermission;

		NotificationParams.m_bInitial = true;
		NotificationParams.m_bAccessDenied = !bHasPermission;

		if (bHasPermission)
		{
			for (auto &pApplication : mp_Applications)
			{
				auto &Application = *pApplication;

				if (auto pPermission = Permissions.f_FindEqual("/App/" + Application.m_Name); !pPermission || !*pPermission)
				{
					pSubscription->m_Filtered[Application.m_Name];
					NotificationParams.m_bFiltered = true;
					continue;
				}

				auto &Change = NotificationParams.m_Changes.f_Insert();
				Change.m_Application = Application.m_Name;
				Change.m_Change = CAppManagerInterface::CApplicationChange_AddOrChangeInfo{fp_GetApplicationInfo(Application)};
			}
		}

		pSubscription->m_bInitialFinished = true;
		pSubscription->m_fOnChange(fg_Move(NotificationParams)) > fg_LogError("ChangeNotifications", "Send initial change notification failed");

		if (bHasPermission)
		{
			for (auto &fOnFinished : pSubscription->m_OnInitialFinished)
				fOnFinished();
		}
		pSubscription->m_OnInitialFinished.f_Clear();

		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_ChangeNotifications_PermissionsChanged()
	{
		return mp_ChangeNotificationsPermissionsChangedSequencer.f_RunSequenced
			(
				g_ActorFunctorWeak / [this](CActorSubscription &&_Subscription) -> TCFuture<void>
				{
					TCActorResultVector<void> SendInitialResults;
					for (auto &Subscription : mp_ChangeNotificationSubscriptions)
						self(&CAppManagerActor::fp_ChangeNotifications_SendInitial, mp_ChangeNotificationSubscriptions.fs_GetKey(Subscription)) > SendInitialResults.f_AddResult();

					co_await (co_await SendInitialResults.f_GetResults() | g_Unwrap);

					(void)_Subscription;

					co_return {};
				}
			)
		;
	}

	TCFuture<void> CAppManagerActor::fp_SyncNotifications(CStr _ApplicationName)
	{
		// Wait 10 seconds for notifications to be delivered to all, ignore errors
		co_await fp_SendAppChange_Empty(_ApplicationName).f_Timeout(10.0, "Timed out syncing notifications").f_Wrap();
		co_return {};
	}

	TCFuture<void> CAppManagerActor::fp_SendAppChange_Empty(CStr _ApplicationName)
	{
		co_return co_await fp_ChangeNotifications_SendChanges({}, _ApplicationName);
	}

	void CAppManagerActor::fp_SendAppChange_Status(CApplication const &_Application)
	{
		fp_ChangeNotifications_SendChanges
			(
				{
					CAppManagerInterface::CChangeNotification
					{
						_Application.m_Name
						, CAppManagerInterface::CApplicationChange_Status{_Application.m_LaunchStatus, _Application.m_LaunchStatusSeverity}
					}
				}
				, _Application.m_Name
			)
			> fg_DiscardResult()
		;
	}

	void CAppManagerActor::fp_SendAppChange_AddedOrChanged(CApplication const &_Application)
	{
		fp_ChangeNotifications_SendChanges
			(
				{
					CAppManagerInterface::CChangeNotification
					{
						_Application.m_Name
						, CAppManagerInterface::CApplicationChange_AddOrChangeInfo{fp_GetApplicationInfo(_Application)}
					}
				}
				, _Application.m_Name
			)
			> fg_DiscardResult()
		;
	}

	void CAppManagerActor::fp_SendAppChange_Removed(CApplication const &_Application)
	{
		fp_ChangeNotifications_SendChanges
			(
				{
					CAppManagerInterface::CChangeNotification
					{
						_Application.m_Name
						, CAppManagerInterface::CApplicationChange_Remove{}
					}
				}
				, _Application.m_Name
			)
			> fg_DiscardResult()
		;
	}

	TCFuture<void> CAppManagerActor::fp_ChangeNotifications_SendChanges(TCVector<CAppManagerInterface::CChangeNotification> _Notifications, CStr _Application)
	{
		TCActorResultVector<void> OnChangeResultsVector;

		CAppManagerInterface::COnChangeNotificationParams NotificationParams;
		NotificationParams.m_Changes = fg_Move(_Notifications);

		CStr AppPermission = fg_Format("AppManager/App/{}", _Application);

		for (auto &Subscription : mp_ChangeNotificationSubscriptions)
		{
			auto &SubscriptionID = mp_ChangeNotificationSubscriptions.fs_GetKey(Subscription);
			auto fSendNotification = [this, SubscriptionID, AppPermission, NotificationParams, _Application]() mutable -> TCFuture<void>
				{
					TCPromise<void> OnChangePromise;

					auto *pSubscription = mp_ChangeNotificationSubscriptions.f_FindEqual(SubscriptionID);
					if (!pSubscription || pSubscription->m_bAccessDenied)
						return OnChangePromise <<= g_Void;

					auto &Subscription = *pSubscription;

					mp_Permissions.f_HasPermission("AppManager Change Event", {"AppManager/AppAll", AppPermission}, Subscription.m_CallingHostInfo)
						.f_Timeout(60.0, "Timed out waiting for permission in OnChange callback to {}"_f << Subscription.m_CallingHostInfo.f_GetRealHostID())
						> OnChangePromise / [=, NotificationParams = fg_Move(NotificationParams)](bool _bHasPermission) mutable
						{
							auto pSubscription = mp_ChangeNotificationSubscriptions.f_FindEqual(SubscriptionID);
							if (!pSubscription)
								return OnChangePromise.f_SetResult();

							bool bWasFiltered = !pSubscription->m_Filtered.f_IsEmpty();
							{
								if (_bHasPermission)
									pSubscription->m_Filtered.f_Remove(_Application);
								else
								{
									if (NotificationParams.m_Changes.f_IsEmpty())
										pSubscription->m_Filtered[_Application];
									else
									{
										for (auto &Change : NotificationParams.m_Changes)
										{
											if (Change.m_Change.f_IsOfType<CAppManagerInterface::CApplicationChange_Remove>())
												pSubscription->m_Filtered.f_Remove(_Application);
											else
												pSubscription->m_Filtered[_Application];
										}
									}

									NotificationParams.m_Changes.f_Clear();
								}
							}

							NotificationParams.m_bFiltered = !pSubscription->m_Filtered.f_IsEmpty();

							if (!_bHasPermission && bWasFiltered == NotificationParams.m_bFiltered)
								return OnChangePromise.f_SetResult();

							pSubscription->m_fOnChange(fg_Move(NotificationParams))
								.f_Timeout(60.0, "Timed out waiting for OnChange callback to {}"_f << pSubscription->m_CallingHostInfo.f_GetRealHostID())
								> OnChangePromise
							;
						}
					;
					return OnChangePromise.f_MoveFuture();
				}
			;
			if (Subscription.m_bInitialFinished)
			{
				if (Subscription.m_bWaitForResult)
					fSendNotification() > OnChangeResultsVector.f_AddResult();
				else
					fSendNotification() > fg_LogWarning("ChangeNotifications", "Send non-waiting change notification failed");
			}
			else
			{
				Subscription.m_OnInitialFinished.f_Insert
					(
						[fSendNotification = fg_Move(fSendNotification)]() mutable
						{
							fSendNotification() > fg_LogWarning("ChangeNotifications", "Deferred change notification failed");
						}
					)
				;
			}
		}

		auto Result = co_await OnChangeResultsVector.f_GetUnwrappedResults().f_Wrap();

		if (!Result)
		{
			DMibLogWithCategory
				(
					Malterlib/Cloud/AppManager
					, Warning
					, "Failed to send change notification: {}"
					, Result.f_GetExceptionStr()
				)
			;
		}

		co_return {};
	}
}
