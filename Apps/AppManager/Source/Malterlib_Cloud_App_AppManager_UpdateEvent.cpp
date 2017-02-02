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
				g_ActorSubscription > [pThis, SubscriptionID, Auditor]
				{
					Auditor.f_Info(fg_Format("Unsubscribe from update notifications '{}'", SubscriptionID));
					pThis->mp_UpdateNotificationSubscriptions.f_Remove(SubscriptionID);
				}
			)
		;
	}

	void CAppManagerActor::fp_OnUpdateEvent
		(
			CStr const &_Application
			, CAppManagerInterface::EUpdateStage _Stage
			, CAppManagerInterface::CVersionIDAndPlatform const &_VersionID
			, NStr::CStr const &_Message
		)
	{
		CAppManagerInterface::CUpdateNotification Notification;
		Notification.m_Application = _Application;
		Notification.m_Message = _Message;
		Notification.m_VersionID = _VersionID;
		Notification.m_Stage = _Stage;
		
		CStr AppPermission = fg_Format("AppManager/App/{}", _Application);
		
		for (auto &Subscription : mp_UpdateNotificationSubscriptions)
		{
			if (!mp_Permissions.f_HostHasAnyPermission(Subscription.m_CallingHostID, "AppManager/AppAll", AppPermission))
				continue;
			
			Subscription.m_fOnUpdate(Notification) > fg_DiscardResult();
		}
	}
}
