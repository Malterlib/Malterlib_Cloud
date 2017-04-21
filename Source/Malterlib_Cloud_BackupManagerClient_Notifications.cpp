// Copyright © 2015 Hansoft AB 
// Distributed under the MIT license, see license text in LICENSE.Malterlib

#include <Mib/Core/Core>
#include <Mib/Cryptography/RandomID>
#include <Mib/Concurrency/ActorSubscription>

#include "Malterlib_Cloud_BackupManagerClient.h"
#include "Malterlib_Cloud_BackupManagerClient_Internal.h"

namespace NMib::NCloud
{
	NConcurrency::TCContinuation<NConcurrency::CActorSubscription> CBackupManagerClient::f_SubscribeNotifications
		(
			ENotification _ToSubscribeTo 
			, NConcurrency::TCActorFunctor<NConcurrency::TCContinuation<void> (NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)> &&_fOnFinished
		)
	{
		auto &Internal = *mp_pInternal;
		CStr SubscriptionID = NCryptography::fg_RandomID();
		
		auto &Subscription = Internal.m_NotificationSubscriptions[SubscriptionID];
		
		Subscription.m_Notifications = _ToSubscribeTo;
		Subscription.m_fOnNotification = fg_Move(_fOnFinished);
		
		return g_Explicit = g_ActorSubscription > [this, SubscriptionID]() -> TCContinuation<void>
			{
				auto &Internal = *mp_pInternal;

				auto *pSubscription = Internal.m_NotificationSubscriptions.f_FindEqual(SubscriptionID);
				if (!pSubscription)
					return fg_Explicit();
				
				auto Continuation = pSubscription->m_fOnNotification.f_Destroy();

				Internal.m_NotificationSubscriptions.f_Remove(SubscriptionID);
				
				return Continuation;
			}
		;
	}

	void CBackupManagerClient::fp_OnNotification(NConcurrency::CHostInfo const &_RemoteHost, CNotification &&_Notification)
	{
		auto &Internal = *mp_pInternal;

		for (auto &Subscription : Internal.m_NotificationSubscriptions)
		{
			if (!(Subscription.m_Notifications & _Notification.f_GetTypeID()))
				continue;
			Subscription.m_fOnNotification(_RemoteHost, fg_TempCopy(_Notification)) > fg_DiscardResult();
		}
	}
}
